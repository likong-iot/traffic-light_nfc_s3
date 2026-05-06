#include "app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "board_hal.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem_4g.h"
#include "net_eth.h"
#include "nfc_pn532.h"
#include "storage_sd.h"

static const char *TAG = "app";

#define APP_WORK_TASK_STACK      8192
#define APP_WORK_TASK_PRIORITY   3
#define APP_WORK_LOG_INTERVAL_MS 10000
#define APP_NFC_POLL_INTERVAL_MS 300
#define APP_IO_PULSE_HIGH_MS     50
#define APP_IO_PULSE_LOW_GAP_MS  50

static TaskHandle_t s_work_task = NULL;

static void bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t pos = 0;
    if (out_len == 0) {
        return;
    }
    out[0] = '\0';

    for (size_t i = 0; i < len && pos < out_len; ++i) {
        int n = snprintf(out + pos, out_len - pos, "%s%02X", i == 0 ? "" : ":", data[i]);
        if (n < 0 || n >= (int)(out_len - pos)) {
            break;
        }
        pos += (size_t)n;
    }
}

static esp_err_t app_append_sd_log(const char *line)
{
    if (line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_sd_is_mounted()) {
        ESP_LOGW(TAG, "[work] SD not mounted, log skipped: %s", line);
        return ESP_ERR_INVALID_STATE;
    }

    char path[64];
    int path_len = snprintf(path, sizeof(path), "%s/NFCLOG.TXT", storage_sd_mount_point());
    if (path_len < 0 || path_len >= (int)sizeof(path)) {
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "[work] open log '%s' failed: errno=%d (%s)",
                 path, errno, strerror(errno));
        return ESP_FAIL;
    }

    if (fprintf(f, "%s\r\n", line) < 0) {
        ESP_LOGE(TAG, "[work] write log failed: errno=%d (%s)", errno, strerror(errno));
        fclose(f);
        return ESP_FAIL;
    }
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(f);
    return ESP_OK;
}

static void app_log_nfc_event(const char *event,
                              const nfc_card_command_t *card,
                              esp_err_t result,
                              int pulse_count)
{
    char uid_hex[3 * 10] = {0};
    char data_hex[3 * 16] = {0};
    char line[256];

    if (card != NULL) {
        bytes_to_hex(card->uid, card->uid_length, uid_hex, sizeof(uid_hex));
        bytes_to_hex(card->data, card->data_length, data_hex, sizeof(data_hex));
    } else {
        strcpy(uid_hex, "-");
        strcpy(data_hex, "-");
    }

    snprintf(line, sizeof(line),
             "tick_ms=%lu,event=%s,uid=%s,source=%s,data=%s,cmd=%d,pulses=%d,result=%s",
             (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
             event ? event : "-",
             uid_hex,
             card && card->source ? card->source : "-",
             data_hex,
             card ? (int)card->command_value : -1,
             pulse_count,
             esp_err_to_name(result));

    ESP_LOGI(TAG, "[work] log: %s", line);
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_append_sd_log(line));
}

static bool same_card_uid(const nfc_card_command_t *card, const uint8_t *last_uid, uint8_t last_uid_len)
{
    return card != NULL &&
           card->uid_length == last_uid_len &&
           memcmp(card->uid, last_uid, card->uid_length) == 0;
}

static void app_work_task(void *arg)
{
    const app_devices_t *devices = (const app_devices_t *)arg;

    ESP_LOGI(TAG, "=== app work task started ===");
    ESP_LOGI(TAG, "Work task owns NFC command loop, IO_OUT1 pulses, SD logging, and periodic status");
    ESP_LOGI(TAG, "[work] NFC command rule: 00=>1 pulse, 01=>2 pulses, 02=>3 pulses");

    uint8_t last_uid[10] = {0};
    uint8_t last_uid_len = 0;
    bool card_latched = false;
    uint32_t last_status_log_ms = 0;

    while (1) {
        if (devices->nfc_ready) {
            nfc_card_command_t card = {0};
            esp_err_t err = nfc_pn532_read_card_command(&card);
            if (err == ESP_OK) {
                if (!card_latched || !same_card_uid(&card, last_uid, last_uid_len)) {
                    int pulse_count = (int)card.command_value + 1;
                    ESP_LOGI(TAG, "[work] NFC command %02u -> IO_OUT1 %d pulse(s)",
                             card.command_value, pulse_count);
                    app_log_nfc_event("card_read", &card, ESP_OK, pulse_count);

                    esp_err_t pulse_err = board_hal_pulse_io_out1(pulse_count,
                                                                  APP_IO_PULSE_HIGH_MS,
                                                                  APP_IO_PULSE_LOW_GAP_MS);
                    app_log_nfc_event("io_out1_pulse", &card, pulse_err, pulse_count);

                    memcpy(last_uid, card.uid, card.uid_length);
                    last_uid_len = card.uid_length;
                    card_latched = true;
                }
            } else if (err == ESP_ERR_NOT_FOUND) {
                if (card_latched) {
                    ESP_LOGI(TAG, "[work] NFC card removed; next card read will trigger again");
                }
                card_latched = false;
                last_uid_len = 0;
                memset(last_uid, 0, sizeof(last_uid));
            } else {
                ESP_LOGW(TAG, "[work] NFC command read failed: %s", esp_err_to_name(err));
                app_log_nfc_event("card_read_failed", &card, err, 0);
            }
        }

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_status_log_ms >= APP_WORK_LOG_INTERVAL_MS) {
            board_inputs_t inputs = {0};
            if (devices->board_ready && board_hal_read_inputs(&inputs) == ESP_OK) {
                ESP_LOGI(TAG, "[work] inputs: KEY1=%d KEY2=%d SD_DET=%d ETH_INT=%d",
                         inputs.key1, inputs.key2, inputs.sd_det, inputs.eth_int);
            }

            ESP_LOGI(TAG, "[work] status: NFC=%s SD=%s ETH=%s 4G=%s",
                     devices->nfc_ready ? "ready" : "unavail",
                     storage_sd_is_mounted() ? "mounted" : "unmounted",
                     net_eth_is_connected() ? "linked" : (devices->eth_ready ? "waiting" : "unavail"),
                     modem_4g_is_connected() ? "connected" : (devices->modem_started ? "connecting" : "paused"));
            last_status_log_ms = now_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_NFC_POLL_INTERVAL_MS));
    }
}

esp_err_t app_work_start(const app_devices_t *devices)
{
    if (devices == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_work_task != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting app work task: stack=%d prio=%d",
             APP_WORK_TASK_STACK, APP_WORK_TASK_PRIORITY);
    BaseType_t ok = xTaskCreatePinnedToCore(app_work_task,
                                            "app_work",
                                            APP_WORK_TASK_STACK,
                                            (void *)devices,
                                            APP_WORK_TASK_PRIORITY,
                                            &s_work_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_work_task = NULL;
        ESP_LOGE(TAG, "Failed to create app work task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
