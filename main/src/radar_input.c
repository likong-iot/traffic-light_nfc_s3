#include "radar_input.h"

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "app.h"
#include "app_config.h"
#include "board_hal.h"
#include "esp_err.h"
#include "esp_log.h"
#include "pin_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "radar_input";

#define RADAR_TASK_STACK      4096
#define RADAR_TASK_PRIORITY    3
#define RADAR_POLL_MS         50
#define RADAR_STATUS_LOG_INTERVAL_MS 5000

typedef struct {
    bool active;
    bool fired;
    bool noisy;
    bool continuous_logged;
    uint32_t cycle_start_ms;
    uint32_t fire_at_ms;
    uint32_t cycle_end_ms;
    uint32_t lockout_until_ms;
    uint32_t last_lockout_log_ms;
    uint32_t noisy_streak;
} radar_filter_state_t;

typedef struct {
    bool last_in1;
    bool last_in2;
    int last_raw_in1;
    int last_raw_in2;
    bool raw_valid;
    uint32_t last_status_log_ms;
} radar_input_state_t;

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static radar_filter_state_t s_filter = {0};

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool trigger_level(const app_radar_config_t *cfg, int level)
{
    return cfg != NULL && level == cfg->active_level;
}

static void clear_cycle_locked(void)
{
    s_filter.active = false;
    s_filter.fired = false;
    s_filter.noisy = false;
    s_filter.continuous_logged = false;
    s_filter.cycle_start_ms = 0;
    s_filter.fire_at_ms = 0;
    s_filter.cycle_end_ms = 0;
}

static void start_cycle_locked(uint32_t now, const app_radar_config_t *cfg)
{
    s_filter.active = true;
    s_filter.fired = false;
    s_filter.noisy = false;
    s_filter.continuous_logged = false;
    s_filter.cycle_start_ms = now;
    s_filter.fire_at_ms = now + (uint32_t)cfg->trigger_delay_ms;
    s_filter.cycle_end_ms = now + (uint32_t)cfg->cycle_window_ms;
}

static bool finish_cycle_locked(uint32_t now, const app_radar_config_t *cfg, bool *lockout_started)
{
    bool was_noisy = s_filter.noisy;
    if (was_noisy) {
        s_filter.noisy_streak++;
    } else {
        s_filter.noisy_streak = 0;
    }

    if (cfg->interference_cycles > 0 &&
        s_filter.noisy_streak >= (uint32_t)cfg->interference_cycles) {
        s_filter.lockout_until_ms = now + (uint32_t)cfg->lockout_ms;
        s_filter.noisy_streak = 0;
        if (lockout_started != NULL) {
            *lockout_started = true;
        }
    }

    clear_cycle_locked();
    return was_noisy;
}

void radar_input_cancel_pending(void)
{
    if (s_state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool had_cycle = s_filter.active;
    clear_cycle_locked();
    xSemaphoreGive(s_state_mutex);

    if (had_cycle) {
        ESP_LOGI(TAG, "radar pending/active filter cycle canceled by NFC priority");
    }
}

static void post_radar_ready(uint32_t timestamp_ms)
{
    app_control_event_t event = {
        .type = APP_CONTROL_EVENT_RADAR_READY,
        .timestamp_ms = timestamp_ms,
    };
    esp_err_t err = app_post_control_event(&event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "radar ready event dropped: %s", esp_err_to_name(err));
    }
}

static void log_periodic_status(const app_radar_config_t *cfg,
                                const board_inputs_t *inputs,
                                bool active_in1,
                                bool active_in2,
                                uint32_t now)
{
    bool filter_active = false;
    bool filter_fired = false;
    bool filter_noisy = false;
    uint32_t noisy_streak = 0;
    uint32_t delay_left = 0;
    uint32_t window_left = 0;
    uint32_t lockout_left = 0;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    filter_active = s_filter.active;
    filter_fired = s_filter.fired;
    filter_noisy = s_filter.noisy;
    noisy_streak = s_filter.noisy_streak;
    if (s_filter.active && !s_filter.fired && !time_reached(now, s_filter.fire_at_ms)) {
        delay_left = s_filter.fire_at_ms - now;
    }
    if (s_filter.active && !time_reached(now, s_filter.cycle_end_ms)) {
        window_left = s_filter.cycle_end_ms - now;
    }
    if (!time_reached(now, s_filter.lockout_until_ms)) {
        lockout_left = s_filter.lockout_until_ms - now;
    }
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG,
             "radar poll status: IN1(GPIO%d)=%d active=%d IN2(GPIO%d)=%d active=%d enabled=%d active_level=%d filter_active=%d fired=%d noisy=%d noisy_streak=%" PRIu32 " delay_left=%" PRIu32 "ms window_left=%" PRIu32 "ms lockout_left=%" PRIu32 "ms",
             PIN_IO_IN1, inputs->io_in1, active_in1,
             PIN_IO_IN2, inputs->io_in2, active_in2,
             cfg->enabled, cfg->active_level,
             filter_active, filter_fired, filter_noisy, noisy_streak,
             delay_left, window_left, lockout_left);
}

static void radar_filter_step(const app_radar_config_t *cfg, bool level_active, uint32_t now)
{
    bool post_ready = false;
    bool log_start = false;
    bool log_continuous = false;
    bool log_noisy_end = false;
    bool log_lockout = false;
    bool log_ignored_lockout = false;
    uint32_t lockout_left = 0;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (!cfg->enabled) {
        clear_cycle_locked();
        xSemaphoreGive(s_state_mutex);
        return;
    }

    if (s_filter.active && time_reached(now, s_filter.cycle_end_ms)) {
        log_noisy_end = finish_cycle_locked(now, cfg, &log_lockout);
    }

    if (s_filter.active && !s_filter.fired && time_reached(now, s_filter.fire_at_ms)) {
        s_filter.fired = true;
        post_ready = true;
    }

    if (level_active) {
        if (!time_reached(now, s_filter.lockout_until_ms)) {
            lockout_left = s_filter.lockout_until_ms - now;
            if (s_filter.last_lockout_log_ms == 0 ||
                time_reached(now, s_filter.last_lockout_log_ms + RADAR_STATUS_LOG_INTERVAL_MS)) {
                s_filter.last_lockout_log_ms = now;
                log_ignored_lockout = true;
            }
        } else if (s_filter.active && !time_reached(now, s_filter.cycle_end_ms)) {
            s_filter.noisy = true;
            if (!s_filter.continuous_logged && time_reached(now, s_filter.cycle_start_ms + RADAR_POLL_MS)) {
                s_filter.continuous_logged = true;
                log_continuous = true;
            }
        } else if (!s_filter.active) {
            start_cycle_locked(now, cfg);
            log_start = true;
        }
    }

    xSemaphoreGive(s_state_mutex);

    if (post_ready) {
        ESP_LOGI(TAG, "radar filtered trigger ready after %dms delay", cfg->trigger_delay_ms);
        post_radar_ready(now);
    }
    if (log_start) {
        ESP_LOGI(TAG, "radar filter cycle started: delay=%dms window=%dms",
                 cfg->trigger_delay_ms, cfg->cycle_window_ms);
    }
    if (log_continuous) {
        ESP_LOGI(TAG, "radar active level still high inside filter window; cycle marked noisy (%dms)",
                 cfg->cycle_window_ms);
    }
    if (log_noisy_end) {
        ESP_LOGI(TAG, "radar noisy cycle finished");
    }
    if (log_lockout) {
        ESP_LOGW(TAG, "radar interference detected; radar paused until +%dms", cfg->lockout_ms);
    }
    if (log_ignored_lockout) {
        ESP_LOGI(TAG, "radar trigger ignored during lockout (%" PRIu32 " ms left)", lockout_left);
    }
}

static void radar_task(void *arg)
{
    (void)arg;
    radar_input_state_t input_state = {
        .last_raw_in1 = -1,
        .last_raw_in2 = -1,
    };
    ESP_LOGI(TAG, "radar input task started: polling IO_IN1=GPIO%d IO_IN2=GPIO%d every %dms",
             PIN_IO_IN1, PIN_IO_IN2, RADAR_POLL_MS);

    while (1) {
        app_config_t cfg_copy;
        app_config_copy(&cfg_copy);
        board_inputs_t inputs = {0};
        if (board_hal_read_inputs(&inputs) == ESP_OK) {
            bool in1 = trigger_level(&cfg_copy.radar, inputs.io_in1);
            bool in2 = trigger_level(&cfg_copy.radar, inputs.io_in2);
            uint32_t now = now_ms();
            bool level_active = in1 || in2;

            if (!input_state.raw_valid ||
                inputs.io_in1 != input_state.last_raw_in1 ||
                inputs.io_in2 != input_state.last_raw_in2 ||
                in1 != input_state.last_in1 ||
                in2 != input_state.last_in2) {
                ESP_LOGI(TAG, "radar input change: IN1(GPIO%d)=%d active=%d IN2(GPIO%d)=%d active=%d",
                         PIN_IO_IN1, inputs.io_in1, in1,
                         PIN_IO_IN2, inputs.io_in2, in2);
                input_state.raw_valid = true;
                input_state.last_raw_in1 = inputs.io_in1;
                input_state.last_raw_in2 = inputs.io_in2;
            }

            radar_filter_step(&cfg_copy.radar, level_active, now);

            if (input_state.last_status_log_ms == 0 ||
                time_reached(now, input_state.last_status_log_ms + RADAR_STATUS_LOG_INTERVAL_MS)) {
                log_periodic_status(&cfg_copy.radar, &inputs, in1, in2, now);
                input_state.last_status_log_ms = now;
            }

            input_state.last_in1 = in1;
            input_state.last_in2 = in2;
        }

        vTaskDelay(pdMS_TO_TICKS(RADAR_POLL_MS));
    }
}

esp_err_t radar_input_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
        if (s_state_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ok = xTaskCreatePinnedToCore(radar_task,
                                            "radar_input",
                                            RADAR_TASK_STACK,
                                            NULL,
                                            RADAR_TASK_PRIORITY,
                                            &s_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
