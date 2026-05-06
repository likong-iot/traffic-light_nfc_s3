#include "time_sync.h"

#include <stdbool.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem_4g.h"

static const char *TAG = "time_sync";

#define TIME_SYNC_TASK_STACK          4096
#define TIME_SYNC_TASK_PRIORITY       2
#define TIME_SYNC_WAIT_PPP_TIMEOUT_MS 180000
#define TIME_SYNC_WAIT_POLL_MS        1000
#define TIME_SYNC_WAIT_SNTP_MS        30000
#define TIME_SYNC_RETRY_DELAY_MS      30000

static TaskHandle_t s_task = NULL;
static volatile bool s_synced = false;
static bool s_sntp_inited = false;

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
    log_current_time("SNTP time synchronized: ");
}

static bool wait_for_ppp_ready(void)
{
    uint32_t waited_ms = 0;

    while (waited_ms <= TIME_SYNC_WAIT_PPP_TIMEOUT_MS) {
        if (modem_4g_is_connected()) {
            ESP_LOGI(TAG, "AIR780ER/PPP is connected; starting SNTP");
            return true;
        }

        if ((waited_ms % 5000) == 0) {
            ESP_LOGI(TAG, "waiting for 4G PPP before SNTP (%u/%u ms)",
                     (unsigned)waited_ms, (unsigned)TIME_SYNC_WAIT_PPP_TIMEOUT_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_WAIT_POLL_MS));
        waited_ms += TIME_SYNC_WAIT_POLL_MS;
    }

    ESP_LOGW(TAG, "4G PPP wait timed out; time sync will retry later");
    return false;
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

static void time_sync_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "=== time sync task started ===");
    ESP_LOGI(TAG, "RTC slow clock should use the external 32.768 kHz crystal on GPIO15/GPIO16");

    while (!s_synced) {
        if (!wait_for_ppp_ready()) {
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
            continue;
        }

        esp_err_t err = start_sntp_once();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
            continue;
        }

        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_WAIT_SNTP_MS));
        if (err == ESP_OK) {
            s_synced = true;
            log_current_time("System time ready: ");
            break;
        }

        ESP_LOGW(TAG, "SNTP sync wait failed: %s; retrying", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
    }

    ESP_LOGI(TAG, "=== time sync task finished ===");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t time_sync_start(void)
{
    if (s_task != NULL || s_synced) {
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

esp_err_t time_sync_get_time(time_t *now)
{
    if (now == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }

    time(now);
    return ESP_OK;
}
