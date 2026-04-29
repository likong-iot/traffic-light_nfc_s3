#include "modem_4g.h"

#include <stdbool.h>
#include <inttypes.h>

#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usbh_modem_board.h"
#include "pin_map.h"

static const char *TAG = "modem_4g";

static bool s_connected = false;
static bool s_dte_up = false;
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

static void modem_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != MODEM_BOARD_EVENT) {
        return;
    }
    switch (id) {
    case MODEM_EVENT_DTE_CONN:
        s_dte_up = true;
        ESP_LOGI(TAG, "  [event] DTE connected — USB enumeration successful");
        break;
    case MODEM_EVENT_DTE_DISCONN:
        s_dte_up = false;
        ESP_LOGW(TAG, "  [event] DTE disconnected — USB link lost");
        break;
    case MODEM_EVENT_SIMCARD_CONN:
        ESP_LOGI(TAG, "  [event] SIM card detected and accessible");
        break;
    case MODEM_EVENT_SIMCARD_DISCONN:
        ESP_LOGW(TAG, "  [event] SIM card missing or PIN error");
        break;
    case MODEM_EVENT_NET_CONN: {
        ESP_LOGI(TAG, "  [event] PPP connected — fetching IP info...");
        esp_netif_t *ppp = esp_netif_get_handle_from_ifkey("PPP_DEF");
        if (ppp) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(ppp, &ip) == ESP_OK && ip.ip.addr != 0) {
                ESP_LOGI(TAG, "  [event] 4G IP:      " IPSTR, IP2STR(&ip.ip));
                ESP_LOGI(TAG, "  [event] 4G gateway: " IPSTR, IP2STR(&ip.gw));
                ESP_LOGI(TAG, "  [event] 4G mask:    " IPSTR, IP2STR(&ip.netmask));
                s_connected = true;
            } else {
                ESP_LOGW(TAG, "  [event] PPP up but IP not yet available");
            }
        } else {
            ESP_LOGW(TAG, "  [event] PPP netif handle not found");
        }
        break;
    }
    case MODEM_EVENT_NET_DISCONN:
        ESP_LOGW(TAG, "  [event] PPP disconnected — waiting for reconnect");
        s_connected = false;
        break;
    default:
        ESP_LOGD(TAG, "  [event] modem event id=%" PRIi32, id);
        break;
    }
}

/*
 * Manual POWERKEY pulse when USB enumeration did not happen automatically.
 * CONFIG_MODEM_POWER_GPIO follows sdkconfig. The AIR780E sample uses
 * inactive=LOW and active=HIGH, so keep the pulse polarity driven by Kconfig.
 */
static void powerkey_kick(void)
{
#if CONFIG_MODEM_POWER_GPIO > 0
    gpio_config_t io_config = {
        .pin_bit_mask = BIT64(CONFIG_MODEM_POWER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  POWERKEY gpio_config GPIO%d failed: %s",
                 CONFIG_MODEM_POWER_GPIO, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "  POWERKEY: GPIO%d  inactive_level=%d  active_level=%d",
             CONFIG_MODEM_POWER_GPIO,
             CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL,
             !CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL);
    ESP_LOGI(TAG, "  POWERKEY step 1/3: ensure inactive (level=%d)",
             CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL);
    gpio_set_level(CONFIG_MODEM_POWER_GPIO, CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL);
    ESP_LOGI(TAG, "  POWERKEY step 2/3: assert active (level=%d) for %d ms",
             !CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL, POWERKEY_ACTIVE_MS);
    gpio_set_level(CONFIG_MODEM_POWER_GPIO, !CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(POWERKEY_ACTIVE_MS));
    ESP_LOGI(TAG, "  POWERKEY step 3/3: release inactive (level=%d), settle %d ms",
             CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL, POWERKEY_SETTLE_MS);
    gpio_set_level(CONFIG_MODEM_POWER_GPIO, CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(POWERKEY_SETTLE_MS));
    ESP_LOGI(TAG, "  POWERKEY pulse done");
#else
    ESP_LOGW(TAG, "  CONFIG_MODEM_POWER_GPIO not set — skipping POWERKEY kick");
#endif
}

static void modem_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "=== AIR780ER 4G modem init task start ===");

    /* Step 1: report sdkconfig */
    ESP_LOGI(TAG, "[1/4] sdkconfig: MODEM_TARGET=AIR780_E"
             "  POWER_GPIO=%d  INACTIVE_LEVEL=%d"
             "  PWRKEY=GPIO%d  RESET=GPIO%d  USB_DP=GPIO%d  USB_DN=GPIO%d",
             CONFIG_MODEM_POWER_GPIO, CONFIG_MODEM_POWER_GPIO_INACTIVE_LEVEL,
             PIN_4G_PWRKEY, PIN_4G_RESET, PIN_4G_USB_DP, PIN_4G_USB_DN);

    /* Step 2: build config */
    modem_config_t cfg = MODEM_DEFAULT_CONFIG();
    cfg.flags |= MODEM_FLAGS_INIT_NOT_BLOCK;
    cfg.handler = modem_event_handler;
    cfg.handler_arg = NULL;
    ESP_LOGI(TAG, "[2/4] Modem config: flags=0x%x (INIT_NOT_BLOCK set)  event handler registered",
             (unsigned)cfg.flags);

    /* Step 3: call modem_board_init */
    ESP_LOGI(TAG, "[3/4] Calling modem_board_init()...");
    esp_err_t err = modem_board_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[3/4] modem_board_init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "=== AIR780ER modem init FAILED ===");
        s_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "[3/4] modem_board_init returned OK (USB OTG handoff in progress)");

    /* Step 4: wait loop — modem connects asynchronously via events */
    ESP_LOGI(TAG, "[4/4] Waiting for PPP: USB kick timeout=%d ms  boot timeout=%d ms",
             POWERKEY_KICK_DELAY_MS, BOOT_TIMEOUT_MS);

    bool kicked = false;
    TickType_t t0 = xTaskGetTickCount();
    uint32_t last_log_ms = 0;

    while (!s_connected) {
        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);

        if (elapsed_ms - last_log_ms >= WAIT_LOG_INTERVAL_MS) {
            ESP_LOGI(TAG, "[4/4] Still waiting: elapsed=%u ms  DTE_up=%d  connected=%d  kicked=%d",
                     elapsed_ms, s_dte_up, s_connected, kicked);
            last_log_ms = elapsed_ms;
        }

        if (!kicked && !s_dte_up && elapsed_ms >= POWERKEY_KICK_DELAY_MS) {
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
