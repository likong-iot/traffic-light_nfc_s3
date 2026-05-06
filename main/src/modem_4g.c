#include "modem_4g.h"

#include <stdbool.h>
#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_modem.h"
#include "pin_map.h"

static const char *TAG = "modem_4g";

static bool s_connected = false;
static bool s_dte_up = false;
static bool s_cdc_installed = false;
static bool s_modem_installed = false;
static TaskHandle_t s_task_handle = NULL;

#define MODEM_TASK_STACK        8192
#define MODEM_TASK_PRIORITY     5
/* If USB hasn't been enumerated within this time, do a manual POWERKEY pulse. */
#define POWERKEY_KICK_DELAY_MS  12000
#define POWERKEY_ACTIVE_MS      1200
#define POWERKEY_SETTLE_MS      8000
#define BOOT_TIMEOUT_MS         120000
/* Interval between "still waiting" progress logs. */
#define WAIT_LOG_INTERVAL_MS    5000
#define USBH_CDC_TASK_STACK     4096
#define USBH_CDC_TASK_PRIORITY  5
#define MODEM_AT_BUFFER_SIZE    512
#define POWERKEY_INACTIVE_LEVEL 1

static const usb_modem_id_t s_usb_modem_id_list[] = {
    {.match_id = {USB_DEVICE_ID_MATCH_VID_PID, 0x19D1, 0x0001}, 2, -1, "Luat Air780E/AIR780ER"},
    {.match_id = {0}},
};

static void on_ppp_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    if (event == NULL) {
        return;
    }

    ESP_LOGI(TAG, "  [event] 4G PPP got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "  [event] 4G gateway:    " IPSTR, IP2STR(&event->ip_info.gw));
    ESP_LOGI(TAG, "  [event] 4G mask:       " IPSTR, IP2STR(&event->ip_info.netmask));
    s_connected = event->ip_info.ip.addr != 0;
    s_dte_up = true;
}

static void on_ppp_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    ESP_LOGW(TAG, "  [event] 4G PPP lost IP");
    s_connected = false;
}

/*
 * Manual POWERKEY pulse when USB enumeration did not happen automatically.
 * AIR780E/AIR780ER PWRKEY is normally pulled high and is triggered by a
 * low-level pulse. If the board adds an inverter, flip POWERKEY_INACTIVE_LEVEL.
 */
static void powerkey_kick(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = BIT64(PIN_4G_PWRKEY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  POWERKEY gpio_config GPIO%d failed: %s",
                 PIN_4G_PWRKEY, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "  POWERKEY: GPIO%d  inactive_level=%d  active_level=%d",
             PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL, !POWERKEY_INACTIVE_LEVEL);
    ESP_LOGI(TAG, "  POWERKEY step 1/3: ensure inactive (level=%d)",
             POWERKEY_INACTIVE_LEVEL);
    gpio_set_level(PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL);
    ESP_LOGI(TAG, "  POWERKEY step 2/3: assert active (level=%d) for %d ms",
             !POWERKEY_INACTIVE_LEVEL, POWERKEY_ACTIVE_MS);
    gpio_set_level(PIN_4G_PWRKEY, !POWERKEY_INACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(POWERKEY_ACTIVE_MS));
    ESP_LOGI(TAG, "  POWERKEY step 3/3: release inactive (level=%d), settle %d ms",
             POWERKEY_INACTIVE_LEVEL, POWERKEY_SETTLE_MS);
    gpio_set_level(PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(POWERKEY_SETTLE_MS));
    ESP_LOGI(TAG, "  POWERKEY pulse done");
}

static void powerkey_idle(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = BIT64(PIN_4G_PWRKEY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  POWERKEY idle gpio_config GPIO%d failed: %s",
                 PIN_4G_PWRKEY, esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL));
    ESP_LOGI(TAG, "  POWERKEY idle prepared: GPIO%d=%d", PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL);
}

static void modem_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "=== AIR780ER 4G modem init task start ===");

    ESP_LOGI(TAG, "[1/4] AIR780ER pins: PWRKEY=GPIO%d inactive=%d RESET=GPIO%d USB_DP=GPIO%d USB_DN=GPIO%d",
             PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL,
             PIN_4G_RESET, PIN_4G_USB_DP, PIN_4G_USB_DN);
    powerkey_idle();

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                                             &on_ppp_got_ip, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                                             &on_ppp_lost_ip, NULL));

    ESP_LOGI(TAG, "[2/4] Installing USB CDC host driver");
    usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = USBH_CDC_TASK_STACK,
        .task_priority = USBH_CDC_TASK_PRIORITY,
        .task_coreid = CONFIG_USBH_TASK_CORE_ID,
        .skip_init_usb_host_driver = false,
    };
    esp_err_t err = usbh_cdc_driver_install(&cdc_config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "[2/4] USB CDC host driver already installed");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "[2/4] usbh_cdc_driver_install failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "=== AIR780ER modem init FAILED ===");
        s_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    } else {
        s_cdc_installed = true;
    }

    ESP_LOGI(TAG, "[3/4] Installing iot_usbh_modem 2.x (VID=0x19D1 PID=0x0001 modem_itf=2)");
    usbh_modem_config_t modem_config = {
        .modem_id_list = s_usb_modem_id_list,
        .at_tx_buffer_size = MODEM_AT_BUFFER_SIZE,
        .at_rx_buffer_size = MODEM_AT_BUFFER_SIZE,
    };
    err = usbh_modem_install(&modem_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[3/4] usbh_modem_install failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "=== AIR780ER modem init FAILED ===");
        s_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_modem_installed = true;
    ESP_LOGI(TAG, "[3/4] iot_usbh_modem installed OK");

    /*
     * iot_usbh_modem 2.x enables PPP auto-connect by default. It will detect
     * USB enumeration, run AT checks, and dial PPP from its modem_daemon task.
     */
    ESP_LOGI(TAG, "[4/4] Waiting for PPP: USB kick timeout=%d ms  boot timeout=%d ms",
             POWERKEY_KICK_DELAY_MS, BOOT_TIMEOUT_MS);

    bool kicked = false;
    TickType_t t0 = xTaskGetTickCount();
    uint32_t last_log_ms = 0;

    while (!s_connected) {
        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);

        if (elapsed_ms - last_log_ms >= WAIT_LOG_INTERVAL_MS) {
            esp_netif_t *ppp = usbh_modem_get_netif();
            esp_netif_ip_info_t ip = {0};
            bool has_ip = ppp != NULL &&
                          esp_netif_get_ip_info(ppp, &ip) == ESP_OK &&
                          ip.ip.addr != 0;
            if (has_ip) {
                ESP_LOGI(TAG, "[4/4] PPP netif has IP: " IPSTR, IP2STR(&ip.ip));
                s_connected = true;
                break;
            }

            ESP_LOGI(TAG, "[4/4] Still waiting: elapsed=%u ms  connected=%d  kicked=%d  netif=%p",
                     elapsed_ms, s_connected, kicked, (void *)ppp);
            last_log_ms = elapsed_ms;
        }

        if (!kicked && elapsed_ms >= POWERKEY_KICK_DELAY_MS) {
            ESP_LOGW(TAG, "[4/4] USB not enumerated after %u ms — issuing POWERKEY kick", elapsed_ms);
            powerkey_kick();
            kicked = true;
        }

        if (elapsed_ms >= BOOT_TIMEOUT_MS) {
            ESP_LOGE(TAG, "[4/4] Boot timeout after %u ms (limit=%d ms)", elapsed_ms, BOOT_TIMEOUT_MS);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (s_connected) {
        ESP_LOGI(TAG, "=== AIR780ER 4G modem ready — PPP up ===");
    } else {
        ESP_LOGE(TAG, "=== AIR780ER 4G modem FAILED to connect ===");
    }

    s_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t modem_4g_init(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Creating modem init task: name=modem_4g  stack=%d  prio=%d  affinity=ANY",
             MODEM_TASK_STACK, MODEM_TASK_PRIORITY);

    BaseType_t r = xTaskCreatePinnedToCore(modem_task, "modem_4g",
                                           MODEM_TASK_STACK, NULL,
                                           MODEM_TASK_PRIORITY,
                                           &s_task_handle, tskNO_AFFINITY);
    if (r != pdPASS) {
        s_task_handle = NULL;
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed (stack=%d) — out of memory?", MODEM_TASK_STACK);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "4G modem task created OK (PWRKEY=GPIO%d  RESET=GPIO%d  USB_DP=GPIO%d  USB_DN=GPIO%d)",
             PIN_4G_PWRKEY, PIN_4G_RESET, PIN_4G_USB_DP, PIN_4G_USB_DN);
    return ESP_OK;
}

bool modem_4g_is_connected(void)
{
    return s_connected;
}
