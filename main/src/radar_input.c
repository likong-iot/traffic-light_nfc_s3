#include "radar_input.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "board_hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "radar_input";

#define RADAR_TASK_STACK      4096
#define RADAR_TASK_PRIORITY    3
#define RADAR_POLL_MS         50
#define RADAR_PULSE_ACTIVE_MS 60
#define RADAR_PULSE_GAP_MS    60

typedef struct {
    bool active;
    uint32_t cycle_start_ms;
    uint32_t fire_at_ms;
    bool fired;
    bool last_in1;
    bool last_in2;
} radar_state_t;

static TaskHandle_t s_task = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool trigger_level(const app_radar_config_t *cfg, int level)
{
    return cfg != NULL && level == cfg->active_level;
}

static void radar_task(void *arg)
{
    (void)arg;
    radar_state_t state = {0};
    ESP_LOGI(TAG, "radar input task started");

    while (1) {
        const app_config_t *cfg = app_config_get();
        board_inputs_t inputs = {0};
        if (board_hal_read_inputs(&inputs) == ESP_OK) {
            bool in1 = trigger_level(&cfg->radar, inputs.io_in1);
            bool in2 = trigger_level(&cfg->radar, inputs.io_in2);
            uint32_t now = now_ms();

            bool rising = (in1 && !state.last_in1) || (in2 && !state.last_in2);
            if (cfg->radar.enabled && rising) {
                if (!state.active || (uint32_t)(now - state.cycle_start_ms) >= (uint32_t)cfg->radar.cycle_window_ms) {
                    state.active = true;
                    state.cycle_start_ms = now;
                    state.fire_at_ms = now + (uint32_t)cfg->radar.trigger_delay_ms;
                    state.fired = false;
                    ESP_LOGI(TAG, "radar cycle started: delay=%d ms window=%d ms", cfg->radar.trigger_delay_ms, cfg->radar.cycle_window_ms);
                } else {
                    ESP_LOGI(TAG, "radar trigger ignored inside %d ms cycle window", cfg->radar.cycle_window_ms);
                }
            }

            if (state.active && !state.fired && (int32_t)(now - state.fire_at_ms) >= 0) {
                ESP_LOGI(TAG, "radar pulse firing: OPTO1+OPTO2 x%d", cfg->radar.opto12_pulses);
                ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_pulse_opto12(cfg->radar.opto12_pulses,
                                                                      RADAR_PULSE_ACTIVE_MS,
                                                                      RADAR_PULSE_GAP_MS));
                state.fired = true;
            }

            if (state.active && (uint32_t)(now - state.cycle_start_ms) >= (uint32_t)cfg->radar.cycle_window_ms) {
                state.active = false;
                state.fired = false;
            }

            state.last_in1 = in1;
            state.last_in2 = in2;
        }

        vTaskDelay(pdMS_TO_TICKS(RADAR_POLL_MS));
    }
}

esp_err_t radar_input_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
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
