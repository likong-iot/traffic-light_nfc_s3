#include "app.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "board_hal.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "modem_4g.h"
#include "net_eth.h"
#include "nfc_pn532.h"
#include "storage_sd.h"
#include "time_sync.h"

static const char *TAG = "app";

#define APP_WORK_TASK_STACK      8192
#define APP_WORK_TASK_PRIORITY   3
#define APP_WORK_LOG_INTERVAL_MS 10000
#define APP_NFC_POLL_INTERVAL_MS 100
#define APP_IO_PULSE_HIGH_MS     50
#define APP_IO_PULSE_LOW_GAP_MS  50

#define APP_LOG_QUEUE_SIZE       64
#define APP_LOG_FLUSH_TASK_STACK 4096
#define APP_LOG_FLUSH_TASK_PRIO  2
#define APP_LOG_FLUSH_INTERVAL_MS (3600 * 1000)
#define APP_LOG_MAX_DAYS         200
#define APP_LOG_DIR              "/NFCLOG"

typedef struct {
    time_t timestamp;
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t data[2];
    char rule_name[32];
} nfc_log_entry_t;

static TaskHandle_t s_work_task = NULL;
static TaskHandle_t s_log_task = NULL;
static QueueHandle_t s_log_queue = NULL;

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

static void app_log_enqueue(const nfc_card_command_t *card, const char *rule_name)
{
    if (s_log_queue == NULL || card == NULL) {
        return;
    }

    nfc_log_entry_t entry = {0};
    time(&entry.timestamp);
    memcpy(entry.uid, card->uid, card->uid_length);
    entry.uid_len = card->uid_length;
    entry.data[0] = card->data[0];
    entry.data[1] = card->data[1];
    if (rule_name != NULL) {
        strlcpy(entry.rule_name, rule_name, sizeof(entry.rule_name));
    }

    if (xQueueSend(s_log_queue, &entry, 0) != pdTRUE) {
        ESP_LOGW(TAG, "[log] queue full, entry dropped");
    }
}

static void app_log_delete_old_files(const char *dir_path, int max_days)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return;
    }

    time_t now = 0;
    time(&now);
    time_t cutoff = now - (time_t)max_days * 86400;

    struct tm cutoff_tm = {0};
    gmtime_r(&cutoff, &cutoff_tm);
    char cutoff_name[32];
    int cy = cutoff_tm.tm_year + 1900;
    int cm = cutoff_tm.tm_mon + 1;
    int cd = cutoff_tm.tm_mday;
    snprintf(cutoff_name, sizeof(cutoff_name), "%04d%02d%02d.TXT", cy, cm, cd);

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strlen(entry->d_name) == 12 && strcmp(entry->d_name, cutoff_name) < 0) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%.12s", dir_path, entry->d_name);
            if (unlink(path) == 0) {
                ESP_LOGI(TAG, "[log] deleted old log: %s", entry->d_name);
            }
        }
    }
    closedir(dir);
}

static void app_log_flush_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[log] flush task started, interval=%d ms", APP_LOG_FLUSH_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(APP_LOG_FLUSH_INTERVAL_MS));

        app_config_t cfg;
        app_config_copy(&cfg);
        if (!cfg.log_enabled) {
            nfc_log_entry_t discard;
            while (xQueueReceive(s_log_queue, &discard, 0) == pdTRUE) {}
            continue;
        }

        if (!storage_sd_is_mounted() || !time_sync_is_synced()) {
            continue;
        }

        char dir_path[64];
        snprintf(dir_path, sizeof(dir_path), "%s%s", storage_sd_mount_point(), APP_LOG_DIR);
        mkdir(dir_path, 0755);

        time_t now = 0;
        time(&now);
        struct tm now_tm = {0};
        localtime_r(&now, &now_tm);
        int fy = now_tm.tm_year + 1900;
        int fm = now_tm.tm_mon + 1;
        int fd = now_tm.tm_mday;
        char file_path[128];
        snprintf(file_path, sizeof(file_path), "%s/%04d%02d%02d.TXT", dir_path, fy, fm, fd);

        nfc_log_entry_t entry;
        int count = 0;
        FILE *f = NULL;

        while (xQueueReceive(s_log_queue, &entry, 0) == pdTRUE) {
            if (f == NULL) {
                f = fopen(file_path, "a");
                if (f == NULL) {
                    ESP_LOGE(TAG, "[log] open '%s' failed: %s", file_path, strerror(errno));
                    break;
                }
            }

            struct tm entry_tm = {0};
            localtime_r(&entry.timestamp, &entry_tm);
            char uid_hex[3 * 10] = {0};
            bytes_to_hex(entry.uid, entry.uid_len, uid_hex, sizeof(uid_hex));

            fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,UID=%s,DATA=%02X%02X,CMD=%s\r\n",
                    entry_tm.tm_year + 1900, entry_tm.tm_mon + 1, entry_tm.tm_mday,
                    entry_tm.tm_hour, entry_tm.tm_min, entry_tm.tm_sec,
                    uid_hex, entry.data[0], entry.data[1], entry.rule_name);
            count++;
        }

        if (f != NULL) {
            fclose(f);
            ESP_LOGI(TAG, "[log] flushed %d entries to %s", count, file_path);
        }

        app_log_delete_old_files(dir_path, APP_LOG_MAX_DAYS);
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
                    } else {
                        ESP_LOGI(TAG, "[work] NFC matched '%s': OPTO1+OPTO2 %d pulse(s), relay4=%s",
                                 rule.name, rule.opto12_pulses,
                                 rule.open_relay4 ? "release" : "no-action");

                        app_log_enqueue(&card, rule.name);

                        board_hal_pulse_opto12(rule.opto12_pulses,
                                              APP_IO_PULSE_HIGH_MS,
                                              APP_IO_PULSE_LOW_GAP_MS);

                        if (rule.open_relay4) {
                            board_hal_set_relay(4, false);
                        }
                    }

                    memcpy(last_uid, card.uid, card.uid_length);
                    last_uid_len = card.uid_length;
                    card_latched = true;
                }
            } else if (err == ESP_ERR_NOT_FOUND) {
                if (card_latched) {
                    ESP_LOGI(TAG, "[work] NFC card removed");
                }
                card_latched = false;
                last_uid_len = 0;
                memset(last_uid, 0, sizeof(last_uid));
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

    s_log_queue = xQueueCreate(APP_LOG_QUEUE_SIZE, sizeof(nfc_log_entry_t));
    if (s_log_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xTaskCreatePinnedToCore(app_log_flush_task, "nfc_log",
                            APP_LOG_FLUSH_TASK_STACK, NULL,
                            APP_LOG_FLUSH_TASK_PRIO, &s_log_task, tskNO_AFFINITY);

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
