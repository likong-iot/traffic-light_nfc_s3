#include "net_eth.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"

static const char *TAG = "net_eth";

static bool s_initialized = false;
static bool s_link_up = false;
static bool s_has_ip = false;
static bool s_isr_installed = false;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static esp_netif_t *s_eth_netif = NULL;
static spi_device_interface_config_t s_devcfg = {
    .command_bits = 0,
    .address_bits = 0,
    .dummy_bits = 0,
    .mode = 0,
    .clock_speed_hz = W5500_SPI_CLOCK_HZ,
    .spics_io_num = PIN_W5500_CS,
    .queue_size = 20,
};

#define W5500_REG_MR       0x0000
#define W5500_REG_VERSIONR 0x0039
#define W5500_MR_RST       0x80
#define W5500_VERSION      0x04
#define W5500_CTRL_READ    0x00
#define W5500_CTRL_WRITE   0x04

static esp_err_t w5500_probe_read_reg(spi_device_handle_t dev, uint16_t reg, uint8_t *value)
{
    spi_transaction_t trans = {
        .flags = SPI_TRANS_USE_RXDATA,
        .cmd = reg,
        .addr = W5500_CTRL_READ,
        .length = 8,
    };
    esp_err_t err = spi_device_polling_transmit(dev, &trans);
    if (err == ESP_OK) {
        *value = trans.rx_data[0];
    }
    return err;
}

static esp_err_t w5500_probe_write_reg(spi_device_handle_t dev, uint16_t reg, uint8_t value)
{
    spi_transaction_t trans = {
        .cmd = reg,
        .addr = W5500_CTRL_WRITE,
        .length = 8,
        .tx_buffer = &value,
    };
    return spi_device_polling_transmit(dev, &trans);
}

static void w5500_hard_reset(void)
{
    ESP_LOGI(TAG, "       Hardware reset: GPIO%d LOW 20 ms -> HIGH 200 ms", PIN_W5500_RST);
    gpio_reset_pin(PIN_W5500_RST);
    gpio_set_direction(PIN_W5500_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_W5500_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_W5500_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static esp_err_t w5500_probe_at_clock(int clock_hz)
{
    spi_device_handle_t dev = NULL;
    spi_device_interface_config_t probe_cfg = {
        .command_bits = 16,
        .address_bits = 8,
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = clock_hz,
        .spics_io_num = PIN_W5500_CS,
        .queue_size = 1,
    };

    esp_err_t err = spi_bus_add_device(W5500_SPI_HOST, &probe_cfg, &dev);
    ESP_RETURN_ON_ERROR(err, TAG, "probe add device failed");

    uint8_t version = 0;
    err = w5500_probe_read_reg(dev, W5500_REG_VERSIONR, &version);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "       probe read VERSIONR failed at %d Hz: %s", clock_hz, esp_err_to_name(err));
        spi_bus_remove_device(dev);
        return err;
    }
    ESP_LOGI(TAG, "       probe VERSIONR at %d Hz: 0x%02x", clock_hz, version);
    if (version != W5500_VERSION) {
        spi_bus_remove_device(dev);
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "       probe software reset at %d Hz", clock_hz);
    err = w5500_probe_write_reg(dev, W5500_REG_MR, W5500_MR_RST);
    if (err != ESP_OK) {
        spi_bus_remove_device(dev);
        return err;
    }
    for (int i = 0; i < 20; ++i) {
        uint8_t mr = 0;
        err = w5500_probe_read_reg(dev, W5500_REG_MR, &mr);
        if (err != ESP_OK) {
            spi_bus_remove_device(dev);
            return err;
        }
        if ((mr & W5500_MR_RST) == 0) {
            ESP_LOGI(TAG, "       probe software reset OK at %d Hz", clock_hz);
            spi_bus_remove_device(dev);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    spi_bus_remove_device(dev);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t w5500_probe_default(void)
{
    w5500_hard_reset();
    ESP_LOGI(TAG, "       probing W5500 VERSIONR at %d Hz", W5500_SPI_CLOCK_HZ);
    ESP_RETURN_ON_ERROR(w5500_probe_at_clock(W5500_SPI_CLOCK_HZ), TAG, "W5500 default probe failed");
    ESP_LOGI(TAG, "       W5500 SPI probe OK");
    return ESP_OK;
}

static void w5500_log_pin_levels(const char *stage)
{
    ESP_LOGI(TAG, "       %s: CS(GPIO%d)=%d RST(GPIO%d)=%d INT(GPIO%d)=%d MOSI(GPIO%d)=%d MISO(GPIO%d)=%d SCLK(GPIO%d)=%d",
             stage,
             PIN_W5500_CS, gpio_get_level(PIN_W5500_CS),
             PIN_W5500_RST, gpio_get_level(PIN_W5500_RST),
             PIN_W5500_INT, gpio_get_level(PIN_W5500_INT),
             PIN_W5500_MOSI, gpio_get_level(PIN_W5500_MOSI),
             PIN_W5500_MISO, gpio_get_level(PIN_W5500_MISO),
             PIN_W5500_SCLK, gpio_get_level(PIN_W5500_SCLK));
}

static esp_err_t w5500_spi_bus_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_W5500_MOSI,
        .miso_io_num = PIN_W5500_MISO,
        .sclk_io_num = PIN_W5500_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };
    return spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
}

static esp_err_t w5500_spi_start(void)
{
    gpio_reset_pin(PIN_W5500_CS);
    gpio_set_direction(PIN_W5500_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_W5500_CS, 1);
    gpio_reset_pin(PIN_W5500_INT);
    gpio_set_direction(PIN_W5500_INT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_W5500_INT, GPIO_PULLUP_ONLY);

    esp_err_t err = w5500_spi_bus_init();
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "       SPI bus already initialized; freeing and retrying");
        spi_bus_free(W5500_SPI_HOST);
        err = w5500_spi_bus_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "SPI bus init failed");

    gpio_set_level(PIN_W5500_CS, 1);
    gpio_set_level(PIN_W5500_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    w5500_log_pin_levels("pin levels before probe");
    ESP_RETURN_ON_ERROR(w5500_probe_default(), TAG, "W5500 SPI probe failed");
    return ESP_OK;
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    uint8_t mac[6] = {0};
    esp_eth_handle_t handle = *(esp_eth_handle_t *)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "W5500 link UP  MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        s_link_up = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "W5500 link DOWN");
        s_link_up = false;
        s_has_ip = false;
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "W5500 eth driver started (waiting for cable)");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "W5500 eth driver stopped");
        s_link_up = false;
        s_has_ip = false;
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "W5500 lost IP");
        s_has_ip = false;
        return;
    }

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "W5500 got IP:   " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "       mask:    " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "       gateway: " IPSTR, IP2STR(&event->ip_info.gw));
    s_link_up = true;
    s_has_ip = event->ip_info.ip.addr != 0;
}

static esp_err_t apply_efuse_mac(esp_eth_handle_t handle)
{
    ESP_LOGI(TAG, "       Reading eFuse base MAC...");
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(mac), TAG, "read efuse MAC failed");
    ESP_LOGI(TAG, "       eFuse base MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    mac[5] ^= 0x02;
    ESP_LOGI(TAG, "       W5500 MAC (byte[5] XOR 0x02): %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(handle, ETH_CMD_S_MAC_ADDR, mac), TAG, "set MAC failed");
    ESP_LOGI(TAG, "       MAC applied to W5500 driver OK");
    return ESP_OK;
}

esp_err_t net_eth_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== W5500 Ethernet init start ===");

    /* Step 1/2: SPI2 bus + W5500 SPI probe */
    ESP_LOGI(TAG, "[1/7] SPI2 bus + W5500 probe: host=%d  MOSI=GPIO%d MISO=GPIO%d SCLK=GPIO%d CS=GPIO%d RST=GPIO%d clock=%d Hz",
             W5500_SPI_HOST, PIN_W5500_MOSI, PIN_W5500_MISO,
             PIN_W5500_SCLK, PIN_W5500_CS, PIN_W5500_RST, W5500_SPI_CLOCK_HZ);
    ESP_RETURN_ON_ERROR(w5500_spi_start(), TAG, "W5500 SPI start failed");
    s_devcfg.clock_speed_hz = W5500_SPI_CLOCK_HZ;

    ESP_LOGI(TAG, "[2/7] W5500 SPI device config: MOSI=GPIO%d  MISO=GPIO%d  CS=GPIO%d  INT=GPIO%d  clock=%d Hz  mode=0  queue=20",
             PIN_W5500_MOSI, PIN_W5500_MISO, PIN_W5500_CS, PIN_W5500_INT, W5500_SPI_CLOCK_HZ);
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &s_devcfg);
    w5500_cfg.int_gpio_num = PIN_W5500_INT;
    ESP_LOGI(TAG, "[2/7] W5500 SPI device config set");

    /* Step 3: GPIO ISR service — LEVEL2 avoids conflict with USB OTG (Level-1). */
    ESP_LOGI(TAG, "[3/7] GPIO ISR service: ESP_INTR_FLAG_LEVEL2 (avoids USB OTG Level-1 conflict)");
    if (!s_isr_installed) {
        esp_err_t r = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL2);
        if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "[3/7] gpio_install_isr_service failed: %s", esp_err_to_name(r));
            return r;
        }
        s_isr_installed = true;
    }
    ESP_LOGI(TAG, "[3/7] GPIO ISR service installed OK");

    /* Step 4: Reset pin handoff. Keep it high and let the PHY driver own HW reset. */
    ESP_LOGI(TAG, "[4/7] RST pin GPIO%d released HIGH; PHY driver will pulse it during install",
             PIN_W5500_RST);
    gpio_set_level(PIN_W5500_RST, 1);

    /* Step 5: MAC and PHY objects */
    ESP_LOGI(TAG, "[5/7] Creating W5500 MAC (host=%d) + PHY (phy_addr=%d  reset_gpio=GPIO%d)",
             W5500_SPI_HOST, W5500_PHY_ADDR, PIN_W5500_RST);
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    mac_cfg.sw_reset_timeout_ms = 1000;
    phy_cfg.phy_addr = W5500_PHY_ADDR;
    phy_cfg.reset_gpio_num = PIN_W5500_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);
    ESP_RETURN_ON_FALSE(mac, ESP_ERR_NO_MEM, TAG, "create W5500 MAC failed");
    ESP_LOGI(TAG, "[5/7] W5500 MAC object created OK");

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!phy) {
        mac->del(mac);
        ESP_LOGE(TAG, "[5/7] create W5500 PHY failed — out of memory");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[5/7] W5500 PHY object created OK");

    /* Step 6: eth driver install + apply eFuse MAC */
    ESP_LOGI(TAG, "[6/7] Installing Ethernet driver (MAC+PHY -> driver handle)");
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_cfg, &s_eth_handle);
    if (err != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        ESP_LOGE(TAG, "[6/7] esp_eth_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "[6/7] Eth driver installed OK (handle=%p)", (void *)s_eth_handle);

    ESP_RETURN_ON_ERROR(apply_efuse_mac(s_eth_handle), TAG, "set eFuse MAC failed");

    /* Step 7: netif glue + netif + attach + events + start */
    ESP_LOGI(TAG, "[7/7] Creating netif glue...");
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(s_eth_glue, ESP_ERR_NO_MEM, TAG, "create netif glue failed");
    ESP_LOGI(TAG, "[7/7] Netif glue created OK");

    ESP_LOGI(TAG, "[7/7] Creating default ETH netif...");
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_eth_netif, ESP_ERR_NO_MEM, TAG, "create netif failed");
    ESP_LOGI(TAG, "[7/7] ETH netif created OK");

    ESP_LOGI(TAG, "[7/7] Attaching glue to netif...");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_glue), TAG, "netif attach failed");
    ESP_LOGI(TAG, "[7/7] Netif attached OK");

    err = esp_netif_dhcpc_start(s_eth_netif);
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED || err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "[7/7] DHCP client already running (%s)", esp_err_to_name(err));
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "DHCP client start failed");
        ESP_LOGI(TAG, "[7/7] DHCP client started OK");
    }

    ESP_LOGI(TAG, "[7/7] Registering ETH_EVENT and IP_EVENT_ETH_GOT_IP handlers...");
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &on_got_ip, NULL));
    ESP_LOGI(TAG, "[7/7] Event handlers registered OK");

    ESP_LOGI(TAG, "[7/7] esp_eth_start()...");
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start failed");
    ESP_LOGI(TAG, "[7/7] esp_eth_start() returned OK");

    s_initialized = true;
    ESP_LOGI(TAG, "=== W5500 Ethernet init done — waiting for link on GPIO%d ===",
             PIN_W5500_INT);
    return ESP_OK;
}

bool net_eth_is_connected(void)
{
    return s_has_ip;
}
