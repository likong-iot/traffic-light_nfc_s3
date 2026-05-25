#include "time_sync.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem_4g.h"
#include "net_eth.h"
#include "pin_map.h"
#include "rtc_rx8025t.h"

static const char *TAG = "time_sync";

#define TIME_SYNC_TASK_STACK          4096
#define TIME_SYNC_TASK_PRIORITY       2
#define TIME_SYNC_WAIT_POLL_MS        1000
#define TIME_SYNC_WAIT_SNTP_MS        30000
#define TIME_SYNC_BOOT_WAIT_FOR_NET_MS 120000
#define TIME_SYNC_RETRY_DELAY_MS      30000
#define TIME_SYNC_DAILY_INTERVAL_MS   (24 * 60 * 60 * 1000UL)

static TaskHandle_t s_task = NULL;
static volatile bool s_synced = false;
static volatile bool s_network_synced = false;
static volatile bool s_rtc_synced = false;
static bool s_sntp_inited = false;

static uint32_t tick_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void log_current_time(const char *prefix)
{
    time_t now = 0;
    struct tm tm_info = {0};
    char text[32] = {0};

    time(&now);
    gmtime_r(&now, &tm_info);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S UTC", &tm_info);
    ESP_LOGI(TAG, "%s%s", prefix, text);
}

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    s_network_synced = true;
    log_current_time("SNTP time synchronized: ");
}

static uint8_t current_network_mask(void)
{
    uint8_t mask = 0;

    if (modem_4g_is_connected()) {
        mask |= 0x01;
    }
    if (net_eth_is_connected()) {
        mask |= 0x02;
    }
    return mask;
}

static const char *network_mask_to_text(uint8_t mask)
{
    switch (mask) {
    case 0x01:
        return "4G PPP";
    case 0x02:
        return "W5500 Ethernet";
    case 0x03:
        return "4G PPP + W5500 Ethernet";
    default:
        return "none";
    }
}

static esp_err_t start_sntp_once(void)
{
    if (s_sntp_inited) {
        ESP_LOGI(TAG, "SNTP already initialized; restarting request");
        return esp_netif_sntp_start();
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.wait_for_sync = true;
    config.sync_cb = on_time_sync;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_sntp_inited = true;
    }
    return err;
}

static esp_err_t sync_time_from_network(void)
{
    uint8_t network_mask = current_network_mask();
    if (network_mask == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "network ready: %s; starting SNTP", network_mask_to_text(network_mask));
    ESP_RETURN_ON_ERROR(start_sntp_once(), TAG, "start_sntp_once failed");

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_WAIT_SNTP_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync wait failed on %s: %s", network_mask_to_text(network_mask), esp_err_to_name(err));
        return err;
    }

    s_synced = true;
    s_network_synced = true;
    log_current_time("System time ready: ");

    esp_err_t save_err = rtc_rx8025t_save_system_time();
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "save SNTP time to RX8025T-UB failed: %s", esp_err_to_name(save_err));
    }
    return ESP_OK;
}

static esp_err_t sync_time_from_rtc(void)
{
    esp_err_t err = rtc_rx8025t_apply_to_system_time();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RX8025T-UB time not used: %s", esp_err_to_name(err));
        return err;
    }

    s_synced = true;
    s_rtc_synced = true;
    log_current_time("System time restored from RX8025T-UB: ");
    return ESP_OK;
}

static esp_err_t wait_for_time_network(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (waited_ms <= timeout_ms) {
        uint8_t network_mask = current_network_mask();
        if (network_mask != 0) {
            ESP_LOGI(TAG, "network ready after wait: %s", network_mask_to_text(network_mask));
            return ESP_OK;
        }

        if ((waited_ms % 5000) == 0) {
            ESP_LOGI(TAG, "waiting for network time source (%" PRIu32 "/%" PRIu32 " ms)",
                     waited_ms, timeout_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_WAIT_POLL_MS));
        waited_ms += TIME_SYNC_WAIT_POLL_MS;
    }

    return ESP_ERR_TIMEOUT;
}

static void wait_until_next_sync(uint32_t *next_sync_ms)
{
    if (next_sync_ms == NULL) {
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_WAIT_POLL_MS));
        return;
    }

    uint32_t now_ms = tick_ms();
    if ((int32_t)(now_ms - *next_sync_ms) >= 0) {
        return;
    }

    uint32_t wait_ms = *next_sync_ms - now_ms;
    if (wait_ms > TIME_SYNC_WAIT_POLL_MS) {
        wait_ms = TIME_SYNC_WAIT_POLL_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
}

esp_err_t time_sync_bootstrap(void)
{
    ESP_LOGI(TAG, "=== time bootstrap start ===");
    ESP_LOGI(TAG, "External RX8025T-UB RTC is on I2C; INT is GPIO%d", PIN_RTC_INT);

    esp_err_t rtc_err = rtc_rx8025t_init();
    if (rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "RX8025T-UB init failed during bootstrap: %s", esp_err_to_name(rtc_err));
    }

    if (current_network_mask() != 0) {
        esp_err_t net_err = sync_time_from_network();
        if (net_err == ESP_OK) {
            ESP_LOGI(TAG, "=== time bootstrap done: network ===");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "bootstrap network sync failed: %s; trying RTC", esp_err_to_name(net_err));
    } else {
        ESP_LOGI(TAG, "bootstrap network unavailable; trying RTC before app work starts");
    }

    if (rtc_err == ESP_OK) {
        esp_err_t rtc_apply_err = sync_time_from_rtc();
        if (rtc_apply_err == ESP_OK) {
            ESP_LOGI(TAG, "=== time bootstrap done: RTC ===");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "bootstrap RTC restore failed: %s", esp_err_to_name(rtc_apply_err));
    }

    ESP_LOGW(TAG, "=== time bootstrap done: no valid time source ===");
    return ESP_ERR_NOT_FOUND;
}

static void time_sync_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "=== time sync task started ===");
    ESP_LOGI(TAG, "External RX8025T-UB RTC is on I2C; INT is GPIO%d", PIN_RTC_INT);

    bool rtc_ready = false;
    esp_err_t rtc_err = rtc_rx8025t_init();
    if (rtc_err == ESP_OK) {
        rtc_ready = true;
        ESP_LOGI(TAG, "RX8025T-UB ready; trying network time first if available");
    } else {
        ESP_LOGW(TAG, "RX8025T-UB init failed: %s", esp_err_to_name(rtc_err));
    }

    uint32_t next_sync_ms = tick_ms();
    if (s_network_synced) {
        next_sync_ms = tick_ms() + TIME_SYNC_DAILY_INTERVAL_MS;
        ESP_LOGI(TAG, "network time already ready; daily network time sync scheduled every 24h");
    } else if (s_rtc_synced) {
        next_sync_ms = tick_ms() + TIME_SYNC_BOOT_WAIT_FOR_NET_MS;
        ESP_LOGI(TAG, "RTC time already restored; will retry network time later");
    } else if (wait_for_time_network(TIME_SYNC_BOOT_WAIT_FOR_NET_MS) == ESP_OK && sync_time_from_network() == ESP_OK) {
        next_sync_ms = tick_ms() + TIME_SYNC_DAILY_INTERVAL_MS;
        ESP_LOGI(TAG, "daily network time sync scheduled every 24h");
    } else if (rtc_ready && sync_time_from_rtc() == ESP_OK) {
        next_sync_ms = tick_ms() + TIME_SYNC_BOOT_WAIT_FOR_NET_MS;
        ESP_LOGI(TAG, "network unavailable or failed; RTC time used, will retry network time later");
    } else {
        next_sync_ms = tick_ms() + TIME_SYNC_RETRY_DELAY_MS;
        ESP_LOGW(TAG, "no valid time source at boot; retrying soon");
    }

    while (1) {
        wait_until_next_sync(&next_sync_ms);

        uint32_t now_ms = tick_ms();
        if ((int32_t)(now_ms - next_sync_ms) < 0) {
            continue;
        }

        esp_err_t err = sync_time_from_network();
        if (err == ESP_OK) {
            next_sync_ms = tick_ms() + TIME_SYNC_DAILY_INTERVAL_MS;
            continue;
        }

        if (rtc_ready) {
            ESP_LOGW(TAG, "network sync failed; falling back to RX8025T-UB");
            err = sync_time_from_rtc();
            if (err == ESP_OK) {
                next_sync_ms = tick_ms() + TIME_SYNC_BOOT_WAIT_FOR_NET_MS;
                continue;
            }
        } else {
            ESP_LOGW(TAG, "network sync failed and RTC is unavailable");
        }

        next_sync_ms = tick_ms() + TIME_SYNC_RETRY_DELAY_MS;
    }
}

esp_err_t time_sync_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(time_sync_task,
                                            "time_sync",
                                            TIME_SYNC_TASK_STACK,
                                            NULL,
                                            TIME_SYNC_TASK_PRIORITY,
                                            &s_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "time sync task created");
    return ESP_OK;
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

bool time_sync_is_network_synced(void)
{
    return s_network_synced;
}

bool time_sync_is_rtc_synced(void)
{
    return s_rtc_synced;
}
