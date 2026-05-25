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
#include "radar_input.h"
#include "storage_sd.h"
#include "time_sync.h"

static const char *TAG = "app";

#define APP_WORK_TASK_STACK      8192
#define APP_WORK_TASK_PRIORITY   3
#define APP_WORK_LOG_INTERVAL_MS 10000
#define APP_WORK_SCHEDULE_INTERVAL_MS 1000
#define APP_NFC_POLL_INTERVAL_MS 100
#define APP_IO_PULSE_HIGH_MS     50
#define APP_IO_PULSE_LOW_GAP_MS  50

#define APP_LOG_QUEUE_SIZE       64
#define APP_LOG_FLUSH_TASK_STACK 4096
#define APP_LOG_FLUSH_TASK_PRIO  2
#define APP_LOG_FLUSH_INTERVAL_MS (3600 * 1000)
#define APP_LOG_MAX_DAYS         200
#define APP_LOG_DIR              "/NFCLOG"
#define APP_CONTROL_QUEUE_SIZE    16

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
static QueueHandle_t s_control_queue = NULL;

typedef enum {
    WORK_SOURCE_NONE = 0,
    WORK_SOURCE_NFC,
    WORK_SOURCE_RADAR,
} work_source_t;

typedef enum {
    WORK_MODE_IDLE = 0,
    WORK_MODE_CLASS1,
    WORK_MODE_CLASS2,
    WORK_MODE_CLASS3_WAIT,
    WORK_MODE_CLASS4_LOCK,
    WORK_MODE_CLASS5_LOCK,
    WORK_MODE_CLASS6_LOCK,
} work_mode_t;

typedef enum {
    LED2_MODE_OFF = 0,
    LED2_MODE_PERMANENT_ON,
    LED2_MODE_CLASS3_HOLD,
} led2_mode_t;

typedef struct {
    work_source_t source;
    work_mode_t mode;
    uint32_t led1_off_at_ms;
    led2_mode_t led2_mode;
    uint32_t led2_hold_until_ms;
    bool class3_tail_pending;
    uint32_t class3_tail_due_ms;
    bool schedule_state_valid;
    bool schedule_wait_logged;
    bool relay1_closed;
    bool relay2_closed;
    uint32_t last_schedule_check_ms;
} app_runtime_state_t;

static app_runtime_state_t s_runtime = {0};

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

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool minute_in_window(int now_min, int start_min, int end_min)
{
    if (start_min == end_min) {
        return true;
    }
    if (start_min < end_min) {
        return now_min >= start_min && now_min < end_min;
    }
    return now_min >= start_min || now_min < end_min;
}

static const char *work_source_name(work_source_t source)
{
    switch (source) {
    case WORK_SOURCE_NONE: return "none";
    case WORK_SOURCE_NFC: return "nfc";
    case WORK_SOURCE_RADAR: return "radar";
    default: return "unknown";
    }
}

static const char *work_mode_name(work_mode_t mode)
{
    switch (mode) {
    case WORK_MODE_IDLE: return "idle";
    case WORK_MODE_CLASS1: return "class1";
    case WORK_MODE_CLASS2: return "class2";
    case WORK_MODE_CLASS3_WAIT: return "class3_wait";
    case WORK_MODE_CLASS4_LOCK: return "class4_lock";
    case WORK_MODE_CLASS5_LOCK: return "class5_lock";
    case WORK_MODE_CLASS6_LOCK: return "class6_lock";
    default: return "unknown";
    }
}

static void runtime_set_work_state(work_source_t source, work_mode_t mode)
{
    if (s_runtime.source != source || s_runtime.mode != mode) {
        ESP_LOGI(TAG, "[state] %s/%s -> %s/%s",
                 work_source_name(s_runtime.source),
                 work_mode_name(s_runtime.mode),
                 work_source_name(source),
                 work_mode_name(mode));
        s_runtime.source = source;
        s_runtime.mode = mode;
    }
}

static bool runtime_has_work_state(void)
{
    return s_runtime.source != WORK_SOURCE_NONE || s_runtime.mode != WORK_MODE_IDLE;
}

static bool runtime_card_allowed(uint8_t class_id)
{
    if (s_runtime.source != WORK_SOURCE_NFC) {
        return true;
    }

    switch (s_runtime.mode) {
    case WORK_MODE_CLASS3_WAIT:
        return class_id == 3 || class_id == 5;
    case WORK_MODE_CLASS4_LOCK:
    case WORK_MODE_CLASS5_LOCK:
    case WORK_MODE_CLASS6_LOCK:
        return class_id == 1;
    default:
        return true;
    }
}

static bool runtime_can_enter_radar_state(void)
{
    return !runtime_has_work_state();
}

static void runtime_apply_relay_schedule(const app_config_t *cfg, uint32_t now, bool force)
{
    if (cfg == NULL) {
        return;
    }
    if (!force && s_runtime.last_schedule_check_ms != 0 &&
        !time_reached(now, s_runtime.last_schedule_check_ms + APP_WORK_SCHEDULE_INTERVAL_MS)) {
        return;
    }
    s_runtime.last_schedule_check_ms = now;

    if (!time_sync_is_synced()) {
        if (!s_runtime.schedule_wait_logged || force) {
            ESP_LOGI(TAG, "[schedule] waiting for time sync before controlling relay1/relay2");
            s_runtime.schedule_wait_logged = true;
        }
        return;
    }

    time_t time_now = 0;
    time(&time_now);
    struct tm tm_info = {0};
    localtime_r(&time_now, &tm_info);
    int now_min = tm_info.tm_hour * 60 + tm_info.tm_min;

    bool relay1_closed = minute_in_window(now_min, cfg->schedule.relay1_start_min, cfg->schedule.relay1_end_min);
    bool relay2_closed = minute_in_window(now_min, cfg->schedule.relay2_start_min, cfg->schedule.relay2_end_min);
    bool changed = !s_runtime.schedule_state_valid ||
                   relay1_closed != s_runtime.relay1_closed ||
                   relay2_closed != s_runtime.relay2_closed;

    if (!s_runtime.schedule_state_valid || relay1_closed != s_runtime.relay1_closed) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(1, relay1_closed));
        s_runtime.relay1_closed = relay1_closed;
    }
    if (!s_runtime.schedule_state_valid || relay2_closed != s_runtime.relay2_closed) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(2, relay2_closed));
        s_runtime.relay2_closed = relay2_closed;
    }
    s_runtime.schedule_state_valid = true;

    if (changed) {
        ESP_LOGI(TAG, "[schedule] %02d:%02d relay1=%s relay2=%s",
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 relay1_closed ? "closed" : "released",
                 relay2_closed ? "closed" : "released");
    }
}

static void runtime_set_led1_off(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led1(false));
    s_runtime.led1_off_at_ms = 0;
}

static void runtime_cancel_class3_tail(void)
{
    s_runtime.class3_tail_pending = false;
    s_runtime.class3_tail_due_ms = 0;
}

static void runtime_set_led2_off(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led2(false));
    s_runtime.led2_mode = LED2_MODE_OFF;
    s_runtime.led2_hold_until_ms = 0;
    runtime_cancel_class3_tail();
}

static void runtime_set_led1_hold(uint32_t now, int hold_ms)
{
    runtime_set_led2_off();

    if (hold_ms <= 0) {
        runtime_set_led1_off();
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led1(true));
    s_runtime.led1_off_at_ms = now + (uint32_t)hold_ms;
}

static void runtime_set_led2_perm_on(void)
{
    runtime_set_led1_off();
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led2(true));
    s_runtime.led2_mode = LED2_MODE_PERMANENT_ON;
    s_runtime.led2_hold_until_ms = 0;
    runtime_cancel_class3_tail();
}

static void runtime_start_led2_hold(uint32_t now, int hold_ms)
{
    if (hold_ms <= 0) {
        runtime_set_led2_off();
        return;
    }

    runtime_set_led1_off();
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led2(true));
    s_runtime.led2_mode = LED2_MODE_CLASS3_HOLD;
    s_runtime.led2_hold_until_ms = now + (uint32_t)hold_ms;
    s_runtime.class3_tail_pending = true;
    s_runtime.class3_tail_due_ms = s_runtime.led2_hold_until_ms;
}

static void runtime_pulse_opto12(int pulse_count)
{
    if (pulse_count <= 0) {
        pulse_count = 1;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_pulse_opto12(pulse_count,
                                                         APP_IO_PULSE_HIGH_MS,
                                                         APP_IO_PULSE_LOW_GAP_MS));
}

static void runtime_enter_class1_state(work_source_t source, const app_config_t *cfg, uint32_t now, int pulse_count)
{
    runtime_set_work_state(source, WORK_MODE_CLASS1);
    runtime_pulse_opto12(pulse_count);
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(4, true));
    runtime_set_led1_hold(now, cfg->timing.class1_led1_hold_ms);
    if (cfg->timing.class1_led1_hold_ms <= 0) {
        runtime_set_work_state(WORK_SOURCE_NONE, WORK_MODE_IDLE);
    }
}

static void runtime_enter_class2_state(work_source_t source, const app_config_t *cfg, uint32_t now, int pulse_count)
{
    runtime_set_work_state(source, WORK_MODE_CLASS2);
    runtime_pulse_opto12(pulse_count);
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(4, true));
    runtime_set_led1_hold(now, cfg->timing.class2_led1_hold_ms);
    if (cfg->timing.class2_led1_hold_ms <= 0) {
        runtime_set_work_state(WORK_SOURCE_NONE, WORK_MODE_IDLE);
    }
}

static void runtime_enter_class3_wait_state(work_source_t source, const app_config_t *cfg, uint32_t now, int pulse_count)
{
    runtime_set_work_state(source, WORK_MODE_CLASS3_WAIT);
    runtime_pulse_opto12(pulse_count);
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(4, true));
    runtime_start_led2_hold(now, cfg->timing.class3_led2_hold_ms);
    if (cfg->timing.class3_led2_hold_ms <= 0) {
        runtime_set_work_state(WORK_SOURCE_NONE, WORK_MODE_IDLE);
    }
}

static void runtime_enter_class4_lock_state(work_source_t source, int pulse_count)
{
    runtime_set_work_state(source, WORK_MODE_CLASS4_LOCK);
    runtime_pulse_opto12(pulse_count);
    runtime_set_led2_perm_on();
}

static void runtime_enter_class5_lock_state(work_source_t source, int pulse_count)
{
    runtime_cancel_class3_tail();
    runtime_set_work_state(source, WORK_MODE_CLASS5_LOCK);
    runtime_pulse_opto12(pulse_count);
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(4, false));
    runtime_set_led2_perm_on();
}

static void runtime_enter_class6_lock_state(work_source_t source, int pulse_count)
{
    runtime_set_work_state(source, WORK_MODE_CLASS6_LOCK);
    runtime_pulse_opto12(pulse_count);
    runtime_set_led2_perm_on();
}

static void runtime_fire_radar_action(const app_config_t *cfg, uint32_t now)
{
    ESP_LOGI(TAG, "[radar] firing class-1-equivalent action: OPTO1+OPTO2 x%d",
             cfg->radar.opto12_pulses);
    runtime_enter_class1_state(WORK_SOURCE_RADAR, cfg, now, cfg->radar.opto12_pulses);
}

static void runtime_apply_card_action(uint8_t class_id,
                                      int pulse_count,
                                      const app_config_t *cfg,
                                      uint32_t now)
{
    if (pulse_count <= 0) {
        pulse_count = (int)class_id;
    }

    switch (class_id) {
    case 1:
        runtime_enter_class1_state(WORK_SOURCE_NFC, cfg, now, pulse_count);
        break;
    case 2:
        runtime_enter_class2_state(WORK_SOURCE_NFC, cfg, now, pulse_count);
        break;
    case 3:
        runtime_enter_class3_wait_state(WORK_SOURCE_NFC, cfg, now, pulse_count);
        break;
    case 4:
        runtime_enter_class4_lock_state(WORK_SOURCE_NFC, pulse_count);
        break;
    case 5:
        runtime_enter_class5_lock_state(WORK_SOURCE_NFC, pulse_count);
        break;
    case 6:
        runtime_enter_class6_lock_state(WORK_SOURCE_NFC, pulse_count);
        break;
    default:
        break;
    }
}

static void runtime_process_timers(const app_config_t *cfg, uint32_t now)
{
    (void)cfg;
    if (s_runtime.led1_off_at_ms != 0 && time_reached(now, s_runtime.led1_off_at_ms)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led1(false));
        s_runtime.led1_off_at_ms = 0;
        if (s_runtime.mode == WORK_MODE_CLASS1 || s_runtime.mode == WORK_MODE_CLASS2) {
            runtime_set_work_state(WORK_SOURCE_NONE, WORK_MODE_IDLE);
        }
    }

    if (s_runtime.led2_mode == LED2_MODE_CLASS3_HOLD &&
        s_runtime.led2_hold_until_ms != 0 &&
        time_reached(now, s_runtime.led2_hold_until_ms)) {
        if (s_runtime.class3_tail_pending && s_runtime.class3_tail_due_ms != 0 &&
            time_reached(now, s_runtime.class3_tail_due_ms)) {
            ESP_LOGI(TAG, "[work] class3 hold expired; firing tail pulse once");
            ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_pulse_opto12(1,
                                                                 APP_IO_PULSE_HIGH_MS,
                                                                 APP_IO_PULSE_LOW_GAP_MS));
        }
        runtime_set_led2_off();
        if (s_runtime.mode == WORK_MODE_CLASS3_WAIT) {
            runtime_set_work_state(WORK_SOURCE_NONE, WORK_MODE_IDLE);
        }
    }
}

static void runtime_handle_radar_ready(uint32_t now, const app_config_t *cfg)
{
    if (!cfg->radar.enabled) {
        return;
    }

    if (!runtime_can_enter_radar_state()) {
        ESP_LOGI(TAG, "[radar] ready event ignored during work state %s/%s",
                 work_source_name(s_runtime.source), work_mode_name(s_runtime.mode));
        return;
    }

    runtime_fire_radar_action(cfg, now);
}

static void runtime_handle_nfc_card(const nfc_card_command_t *card,
                                    const app_config_t *cfg,
                                    uint32_t now)
{
    if (card == NULL) {
        return;
    }

    app_nfc_rule_t rule = {0};
    uint8_t class_id = 0;
    esp_err_t rule_err = app_config_find_nfc_action(card->data[0], card->data[1], &rule, &class_id);
    int pulse_count = 0;
    const char *rule_name = NULL;

    if (rule_err == ESP_OK && class_id >= 1 && class_id <= APP_CONFIG_CARD_CLASS_COUNT) {
        pulse_count = rule.opto12_pulses;
        rule_name = rule.name;
    } else if (card->command_value <= 5) {
        class_id = (uint8_t)card->command_value + 1;
        pulse_count = class_id;
        ESP_LOGW(TAG, "[work] NFC data[0..1]=%02X %02X not in configured class rows; using legacy command %u",
                 card->data[0], card->data[1], card->command_value);
    } else {
        ESP_LOGW(TAG, "[work] unsupported NFC data[0..1]=%02X %02X legacy_command=%u",
                 card->data[0], card->data[1], card->command_value);
        return;
    }

    if (!runtime_card_allowed(class_id)) {
        ESP_LOGI(TAG, "[state] %s/%s ignores NFC class %u",
                 work_source_name(s_runtime.source), work_mode_name(s_runtime.mode), class_id);
        return;
    }

    radar_input_cancel_pending();

    if (rule_name == NULL || rule_name[0] == '\0') {
        ESP_LOGI(TAG, "[work] NFC class %u from legacy command", class_id);
    } else {
        ESP_LOGI(TAG, "[work] NFC matched '%s' -> class %u", rule_name, class_id);
    }

    runtime_apply_card_action(class_id, pulse_count, cfg, now);
    app_log_enqueue(card, rule_name);
}

static void runtime_handle_control_events(const app_config_t *cfg, uint32_t now)
{
    if (s_control_queue == NULL) {
        return;
    }

    app_control_event_t event;
    while (xQueueReceive(s_control_queue, &event, 0) == pdTRUE) {
        if (event.type == APP_CONTROL_EVENT_RADAR_READY) {
            runtime_handle_radar_ready(event.timestamp_ms, cfg);
        }
    }
}

static void app_work_task(void *arg)
{
    const app_devices_t *devices = (const app_devices_t *)arg;

    ESP_LOGI(TAG, "=== app work task started ===");
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_ledb(true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_led1(false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(3, false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(4, true));
    runtime_set_led2_off();

    app_config_t cfg_cache;

    uint8_t last_uid[10] = {0};
    uint8_t last_uid_len = 0;
    bool card_latched = false;
    uint32_t last_status_log_ms = 0;

    while (1) {
        app_config_copy(&cfg_cache);

        uint32_t now = now_ms();
        runtime_apply_relay_schedule(&cfg_cache, now, false);
        runtime_handle_control_events(&cfg_cache, now);
        runtime_process_timers(&cfg_cache, now);

        if (devices->nfc_ready) {
            nfc_card_command_t card = {0};
            esp_err_t err = nfc_pn532_read_card_command(&card);
            if (err == ESP_OK) {
                if (!card_latched || !same_card_uid(&card, last_uid, last_uid_len)) {
                    runtime_handle_nfc_card(&card, &cfg_cache, now_ms());

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

        now = now_ms();
        if (now - last_status_log_ms >= APP_WORK_LOG_INTERVAL_MS) {
            ESP_LOGI(TAG, "[work] status: STATE=%s/%s NFC=%s SD=%s ETH=%s 4G=%s",
                     work_source_name(s_runtime.source),
                     work_mode_name(s_runtime.mode),
                     devices->nfc_ready ? "ready" : "unavail",
                     storage_sd_is_mounted() ? "mounted" : "unmounted",
                     net_eth_is_connected() ? "linked" : (devices->eth_ready ? "waiting" : "unavail"),
                     modem_4g_is_connected() ? "connected" : (devices->modem_started ? "connecting" : "paused"));
            last_status_log_ms = now;
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

    s_control_queue = xQueueCreate(APP_CONTROL_QUEUE_SIZE, sizeof(app_control_event_t));
    if (s_control_queue == NULL) {
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

esp_err_t app_post_control_event(const app_control_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_control_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_control_queue, event, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
