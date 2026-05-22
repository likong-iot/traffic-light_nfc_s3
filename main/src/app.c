#include "app.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"
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
#define APP_NFC_POLL_INTERVAL_MS 100
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

    esp_err_t log_err = app_append_sd_log(line);
    if (log_err != ESP_OK && log_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "[work] SD log write skipped/failed: %s", esp_err_to_name(log_err));
    }
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_ledb(true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led1(true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(3, false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(4, true));
    ESP_LOGI(TAG, "Work task owns NFC command loop, OPTO1+OPTO2 pulses, LEDB/LED1 status, relay defaults, SD logging, and status");
    ESP_LOGI(TAG, "[work] NFC command rules are loaded from app_config (SD -> NVS -> defaults)");
    ESP_LOGI(TAG, "[work] startup relay rule: relay1/2 schedule-controlled, relay3 RELEASED, relay4 CLOSED");

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
                    app_nfc_rule_t rule = {0};
                    esp_err_t match_err = app_config_find_nfc_action(card.data[0], card.data[1], &rule);
                    if (match_err != ESP_OK) {
                        ESP_LOGW(TAG, "[work] unsupported NFC data[0..1]=%02X %02X", card.data[0], card.data[1]);
                        app_log_nfc_event("card_data_unsupported", &card, ESP_ERR_NOT_SUPPORTED, 0);
                        continue;
                    }

                    ESP_LOGI(TAG, "[work] NFC data[0..1]=%02X %02X matched '%s': OPTO1+OPTO2 %d pulse(s), relay4=%s",
                             card.data[0], card.data[1], rule.name, rule.opto12_pulses,
                             rule.open_relay4 ? "release" : "no-action");
                    app_log_nfc_event("card_read", &card, ESP_OK, rule.opto12_pulses);

                    esp_err_t pulse_err = board_hal_pulse_opto12(rule.opto12_pulses,
                                                                 APP_IO_PULSE_HIGH_MS,
                                                                 APP_IO_PULSE_LOW_GAP_MS);
                    app_log_nfc_event("opto12_pulse", &card, pulse_err, rule.opto12_pulses);

                    if (rule.open_relay4) {
                        esp_err_t relay_err = board_hal_set_relay(4, false);
                        app_log_nfc_event("relay4_open", &card, relay_err, 0);
                    }

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
