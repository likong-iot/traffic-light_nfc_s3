#include "ota_update.h"

#include <stdbool.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ota_update";

#define OTA_TASK_STACK     8192
#define OTA_TASK_PRIORITY  4
#define OTA_HTTP_TIMEOUT_MS 30000

static ota_status_t s_status = {.state = OTA_STATE_IDLE};
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_task = NULL;
static char s_url[256];

static void ensure_mutex(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

static void status_set(ota_state_t state, int downloaded, int total, const char *msg)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = state;
    s_status.bytes_downloaded = downloaded;
    s_status.bytes_total = total;
    if (msg != NULL) {
        strlcpy(s_status.message, msg, sizeof(s_status.message));
    }
    xSemaphoreGive(s_mutex);
}

static void status_update_progress(int downloaded, int total)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.bytes_downloaded = downloaded;
    if (total > 0) {
        s_status.bytes_total = total;
    }
    xSemaphoreGive(s_mutex);
}

void ota_update_get_status(ota_status_t *out)
{
    if (out == NULL) {
        return;
    }
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

const char *ota_state_name(ota_state_t state)
{
    switch (state) {
    case OTA_STATE_IDLE:        return "idle";
    case OTA_STATE_DOWNLOADING: return "downloading";
    case OTA_STATE_VERIFYING:   return "verifying";
    case OTA_STATE_SUCCESS:     return "success";
    case OTA_STATE_FAILED:      return "failed";
    default:                    return "unknown";
    }
}

const char *ota_running_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}

static void ota_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "OTA task starting; url=%s", s_url);
    status_set(OTA_STATE_DOWNLOADING, 0, 0, "正在连接服务器...");

    esp_http_client_config_t http_cfg = {
        .url = s_url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        char msg[128];
        snprintf(msg, sizeof(msg), "连接失败: %s", esp_err_to_name(err));
        status_set(OTA_STATE_FAILED, 0, 0, msg);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int total_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "OTA image size: %d bytes", total_size);
    status_set(OTA_STATE_DOWNLOADING, 0, total_size, "下载中...");

    int last_logged = 0;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int got = esp_https_ota_get_image_len_read(handle);
        status_update_progress(got, total_size);
        if (got - last_logged >= 65536) {
            ESP_LOGI(TAG, "OTA progress: %d / %d", got, total_size);
            last_logged = got;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        char msg[128];
        snprintf(msg, sizeof(msg), "下载失败: %s", esp_err_to_name(err));
        status_set(OTA_STATE_FAILED, esp_https_ota_get_image_len_read(handle), total_size, msg);
        esp_https_ota_abort(handle);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA incomplete download");
        status_set(OTA_STATE_FAILED, esp_https_ota_get_image_len_read(handle), total_size, "下载不完整");
        esp_https_ota_abort(handle);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    status_set(OTA_STATE_VERIFYING, total_size, total_size, "校验中...");
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        char msg[128];
        snprintf(msg, sizeof(msg), "校验失败: %s", esp_err_to_name(err));
        status_set(OTA_STATE_FAILED, total_size, total_size, msg);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA finished successfully; restarting in 2s");
    status_set(OTA_STATE_SUCCESS, total_size, total_size, "升级成功，2秒后重启");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

esp_err_t ota_update_start(const char *url)
{
    if (url == NULL || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(url) >= sizeof(s_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = s_status.state == OTA_STATE_DOWNLOADING ||
                s_status.state == OTA_STATE_VERIFYING;
    xSemaphoreGive(s_mutex);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(s_url, url, sizeof(s_url));
    status_set(OTA_STATE_DOWNLOADING, 0, 0, "启动 OTA 任务...");

    BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "ota_update",
                                            OTA_TASK_STACK, NULL,
                                            OTA_TASK_PRIORITY, &s_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_task = NULL;
        status_set(OTA_STATE_FAILED, 0, 0, "任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
