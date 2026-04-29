#include "net_eth.h"

#include <inttypes.h>
#include <string.h>

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

static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static esp_netif_t *s_eth_netif = NULL;
static bool s_started = false;

static void w5500_hw_reset_pulse(void)
{
    /* RSTn is active-low on W5500. Give the chip a clean boot window. */
    gpio_set_level(PIN_W5500_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(PIN_W5500_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void on_eth_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "W5500 link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "W5500 link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet driver started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet driver stopped");
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "ETH got IP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETH netmask:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETH gw:" IPSTR, IP2STR(&ip_info->gw));
}

esp_err_t net_eth_init(void)
{
#if !CONFIG_ETH_SPI_ETHERNET_W5500
    ESP_LOGW(TAG, "CONFIG_ETH_SPI_ETHERNET_W5500 is disabled, skip ethernet init");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_eth_handle != NULL) {
        return ESP_OK;
    }

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_W5500_MISO,
        .mosi_io_num = PIN_W5500_MOSI,
        .sclk_io_num = PIN_W5500_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2048,
    };
    err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = 4 * 1000 * 1000,
        .spics_io_num = PIN_W5500_CS,
        .queue_size = 20,
    };

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = PIN_W5500_RST;

    w5500_hw_reset_pulse();

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
    /*
     * Start in polling mode first to decouple bring-up from INT wiring/polarity.
     * Once stable on hardware, this can be switched back to interrupt mode.
     */
    w5500_config.int_gpio_num = -1;
    w5500_config.poll_period_ms = 100;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_NO_MEM, TAG, "create W5500 MAC failed");

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        ESP_LOGE(TAG, "create W5500 PHY failed");
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t eth_mac[6] = {0};
    err = esp_read_mac(eth_mac, ESP_MAC_ETH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_read_mac failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set ETH MAC failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (s_eth_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_glue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = esp_netif_attach(s_eth_netif, s_eth_glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register ETH_EVENT failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "W5500 ready SPI2 host, pins: MOSI=%d MISO=%d SCLK=%d CS=%d INT=%d RST=%d",
             PIN_W5500_MOSI,
             PIN_W5500_MISO,
             PIN_W5500_SCLK,
             PIN_W5500_CS,
             PIN_W5500_INT,
             PIN_W5500_RST);
    return ESP_OK;

cleanup:
    if (s_eth_glue != NULL) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
    if (s_eth_netif != NULL) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }
    if (s_eth_handle != NULL) {
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }
    return err;
#endif
}

esp_err_t net_eth_start(void)
{
#if !CONFIG_ETH_SPI_ETHERNET_W5500
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_eth_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    return ESP_OK;
#endif
}

esp_err_t net_eth_stop(void)
{
#if !CONFIG_ETH_SPI_ETHERNET_W5500
    return ESP_OK;
#else
    if (s_eth_handle == NULL) {
        return ESP_OK;
    }

    if (s_started) {
        esp_err_t err = esp_eth_stop(s_eth_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_eth_stop failed: %s", esp_err_to_name(err));
            return err;
        }
        s_started = false;
    }

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip);
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event);

    if (s_eth_glue != NULL) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
    if (s_eth_netif != NULL) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }

    esp_err_t err = esp_eth_driver_uninstall(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_uninstall failed: %s", esp_err_to_name(err));
        return err;
    }
    s_eth_handle = NULL;

    spi_bus_free(SPI2_HOST);
    return ESP_OK;
#endif
}

bool net_eth_is_started(void)
{
    return s_started;
}
