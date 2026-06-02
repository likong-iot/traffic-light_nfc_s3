// ============================================================================
// 雷达输入处理模块 - 完全重构版
// 修复日期: 2026-06-02
//
// 新需求规格:
//   1. 保留5秒延时确认 (trigger_delay_ms)
//   2. 20秒窗口去重 (cycle_window_ms)
//   3. 统计累计高电平时间 (high_level_threshold_ms)
//   4. 统计触发次数 (trigger_count_threshold)
//   5. 两种干扰检测条件
//   6. 连续干扰锁定机制
//
// 工作流程:
//   GPIO引脚 → [中断+轮询] → 状态机过滤 → 向状态机发送信号 (仅IDLE接收)
// ============================================================================

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
#include "time_utils.h"  // 使用64位时间戳

static const char *TAG = "radar_input";

#define RADAR_TASK_STACK      4096
#define RADAR_TASK_PRIORITY    3
#define RADAR_POLL_MS         50
#define RADAR_STATUS_LOG_INTERVAL_MS 5000

// ============================================================================
// 雷达状态机枚举
// ============================================================================
typedef enum {
    RADAR_STATE_IDLE,           // 空闲，等待触发
    RADAR_STATE_DEBOUNCING,     // 延时确认中（5秒）
    RADAR_STATE_WINDOW_ACTIVE,  // 窗口激活中（20秒去重）
    RADAR_STATE_LOCKED_OUT,     // 干扰锁定（10分钟）
} radar_state_t;

// ============================================================================
// 雷达过滤状态结构 - 重构版
// ============================================================================
typedef struct {
    // 状态机
    radar_state_t state;
    uint64_t state_enter_ms;        // 进入当前状态的时间

    // 周期统计
    uint64_t cycle_start_ms;        // 本周期开始时间
    int trigger_count;              // 本周期触发次数（电平变化次数）
    bool last_level_active;         // 上次电平状态

    // 高电平时间统计
    uint64_t high_level_start_ms;   // 高电平开始时间（0=当前低电平）
    uint64_t accumulated_high_ms;   // 本周期累计高电平时间

    // 干扰检测
    int interference_streak;        // 连续干扰周期计数
    uint64_t lockout_until_ms;      // 锁定截止时间

    // 日志控制
    uint64_t last_status_log_ms;
    uint64_t last_lockout_log_ms;
} radar_filter_t;

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static radar_filter_t s_filter = {0};

// ============================================================================
// 工具函数
// ============================================================================
static bool trigger_level(const app_radar_config_t *cfg, int level)
{
    return cfg != NULL && level == cfg->active_level;
}

// ============================================================================
// 清空过滤状态
// ============================================================================
static void clear_filter_locked(void)
{
    s_filter.state = RADAR_STATE_IDLE;
    s_filter.state_enter_ms = 0;
    s_filter.cycle_start_ms = 0;
    s_filter.trigger_count = 0;
    s_filter.last_level_active = false;
    s_filter.high_level_start_ms = 0;
    s_filter.accumulated_high_ms = 0;
}

// ============================================================================
// NFC优先权：取消待处理的雷达周期
// ============================================================================
void radar_input_cancel_pending(void)
{
    if (s_state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool had_cycle = (s_filter.state == RADAR_STATE_DEBOUNCING ||
                      s_filter.state == RADAR_STATE_WINDOW_ACTIVE);
    if (had_cycle) {
        clear_filter_locked();
    }
    xSemaphoreGive(s_state_mutex);

    if (had_cycle) {
        ESP_LOGI(TAG, "radar pending/active cycle canceled by NFC priority");
    }
}

// ============================================================================
// 向应用状态机发送雷达就绪事件 - 改进队列满处理
// 修复日期: 2026-06-02
// 修复: 队列满时重试3次，避免丢失触发信号
// ============================================================================
static void post_radar_ready(uint64_t timestamp_ms)
{
    app_control_event_t event = {
        .type = APP_CONTROL_EVENT_RADAR_READY,
        .timestamp_ms = timestamp_ms,
    };

    // 修复: 重试机制，避免队列满时直接丢弃
    esp_err_t err;
    for (int retry = 0; retry < 3; retry++) {
        err = app_post_control_event(&event);
        if (err == ESP_OK) {
            return;  // 发送成功
        }

        if (retry < 2) {  // 不是最后一次重试
            ESP_LOGW(TAG, "radar ready event queue full (attempt %d/3), retrying...", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(10));  // 短暂延时后重试
        }
    }

    // 重试3次后仍失败
    ESP_LOGE(TAG, "radar ready event dropped after 3 retries: %s", esp_err_to_name(err));
}

// ============================================================================
// 雷达过滤状态机 - 核心逻辑
// 修复日期: 2026-06-02
// 实现: 延时确认 + 窗口去重 + 累计高电平统计 + 触发次数统计 + 干扰检测
// ============================================================================
static void radar_filter_step(const app_radar_config_t *cfg, bool level_active, uint64_t now)
{
    if (!cfg->enabled) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        clear_filter_locked();
        xSemaphoreGive(s_state_mutex);
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    radar_state_t old_state = s_filter.state;

    switch (s_filter.state) {
    case RADAR_STATE_IDLE:
        // 空闲状态：等待触发
        if (time_reached_64(now, s_filter.lockout_until_ms)) {
            // 锁定期已结束
            if (level_active) {
                // 检测到高电平，开始新周期
                s_filter.state = RADAR_STATE_DEBOUNCING;
                s_filter.state_enter_ms = now;
                s_filter.cycle_start_ms = now;
                s_filter.trigger_count = 1;  // 第一次触发
                s_filter.last_level_active = true;
                s_filter.high_level_start_ms = now;  // 开始计时高电平
                s_filter.accumulated_high_ms = 0;

                ESP_LOGI(TAG, "[IDLE→DEBOUNCING] cycle started, delay=%dms window=%dms",
                         cfg->trigger_delay_ms, cfg->cycle_window_ms);
            }
        } else {
            // 仍在锁定期
            if (level_active) {
                uint64_t lockout_left = s_filter.lockout_until_ms - now;
                if (s_filter.last_lockout_log_ms == 0 ||
                    time_reached_64(now, s_filter.last_lockout_log_ms + RADAR_STATUS_LOG_INTERVAL_MS)) {
                    s_filter.last_lockout_log_ms = now;
                    ESP_LOGI(TAG, "[LOCKED] trigger ignored, lockout %" PRIu64 " ms left", lockout_left);
                }
            }
        }
        break;

    case RADAR_STATE_DEBOUNCING:
        // 延时确认状态：统计高电平时间和触发次数

        // 统计高电平时间
        if (level_active) {
            // 持续高电平，继续计时
        } else {
            // 变为低电平，累计之前的高电平时间
            if (s_filter.high_level_start_ms != 0) {
                s_filter.accumulated_high_ms += (now - s_filter.high_level_start_ms);
                s_filter.high_level_start_ms = 0;
            }
        }

        // 统计触发次数（电平变化）
        if (level_active != s_filter.last_level_active) {
            if (level_active) {
                s_filter.trigger_count++;
                s_filter.high_level_start_ms = now;  // 重新开始计时
                ESP_LOGI(TAG, "[DEBOUNCING] trigger_count=%d", s_filter.trigger_count);
            }
        }
        s_filter.last_level_active = level_active;

        // 检查延时是否到达
        uint64_t elapsed = now - s_filter.state_enter_ms;
        if (elapsed >= (uint64_t)cfg->trigger_delay_ms) {
            // 修复: 在转换到WINDOW_ACTIVE前，先累加当前高电平时间
            if (s_filter.high_level_start_ms != 0) {
                s_filter.accumulated_high_ms += (now - s_filter.high_level_start_ms);
                s_filter.high_level_start_ms = now;  // 重置起点继续计时
            }

            // 延时确认完成，发送触发信号
            ESP_LOGI(TAG, "[DEBOUNCING→WINDOW_ACTIVE] trigger confirmed after %dms delay, accumulated_high=%" PRIu64 "ms",
                     cfg->trigger_delay_ms, s_filter.accumulated_high_ms);
            post_radar_ready(now);
            s_filter.state = RADAR_STATE_WINDOW_ACTIVE;
            s_filter.state_enter_ms = now;
        }
        break;

    case RADAR_STATE_WINDOW_ACTIVE:
        // 窗口激活状态：继续统计，20秒后判断干扰

        // 继续统计高电平时间
        if (level_active) {
            // 持续高电平
            if (s_filter.high_level_start_ms == 0) {
                s_filter.high_level_start_ms = now;
            }
        } else {
            // 变为低电平
            if (s_filter.high_level_start_ms != 0) {
                s_filter.accumulated_high_ms += (now - s_filter.high_level_start_ms);
                s_filter.high_level_start_ms = 0;
            }
        }

        // 继续统计触发次数
        if (level_active != s_filter.last_level_active) {
            if (level_active) {
                s_filter.trigger_count++;
                s_filter.high_level_start_ms = now;
                ESP_LOGI(TAG, "[WINDOW_ACTIVE] trigger_count=%d", s_filter.trigger_count);
            }
        }
        s_filter.last_level_active = level_active;

        // 检查窗口是否结束
        uint64_t cycle_elapsed = now - s_filter.cycle_start_ms;
        if (cycle_elapsed >= (uint64_t)cfg->cycle_window_ms) {
            // 窗口结束，计算最终的累计高电平时间
            uint64_t final_accumulated = s_filter.accumulated_high_ms;
            if (s_filter.high_level_start_ms != 0) {
                final_accumulated += (now - s_filter.high_level_start_ms);
            }

            // 判断是否为干扰周期
            bool is_interference = false;

            // 条件A：累计高电平时间
            if (final_accumulated >= (uint64_t)cfg->high_level_threshold_ms) {
                is_interference = true;
                ESP_LOGW(TAG, "[INTERFERENCE] accumulated_high=%" PRIu64 " ms >= threshold=%d ms",
                         final_accumulated, cfg->high_level_threshold_ms);
            }

            // 条件B：触发次数
            if (s_filter.trigger_count >= cfg->trigger_count_threshold) {
                is_interference = true;
                ESP_LOGW(TAG, "[INTERFERENCE] trigger_count=%d >= threshold=%d",
                         s_filter.trigger_count, cfg->trigger_count_threshold);
            }

            // 更新干扰计数
            if (is_interference) {
                s_filter.interference_streak++;
                ESP_LOGW(TAG, "[WINDOW_ACTIVE] noisy cycle finished, interference_streak=%d",
                         s_filter.interference_streak);

                // 检查是否需要锁定
                if (s_filter.interference_streak >= cfg->interference_cycles) {
                    s_filter.state = RADAR_STATE_LOCKED_OUT;
                    s_filter.lockout_until_ms = now + (uint64_t)cfg->lockout_ms;
                    s_filter.interference_streak = 0;
                    ESP_LOGW(TAG, "[WINDOW_ACTIVE→LOCKED] %d consecutive interference cycles detected, locked for %dms",
                             cfg->interference_cycles, cfg->lockout_ms);
                } else {
                    clear_filter_locked();
                    ESP_LOGI(TAG, "[WINDOW_ACTIVE→IDLE] cycle ended (interference %d/%d)",
                             s_filter.interference_streak, cfg->interference_cycles);
                }
            } else {
                s_filter.interference_streak = 0;
                clear_filter_locked();
                ESP_LOGI(TAG, "[WINDOW_ACTIVE→IDLE] clean cycle finished (accumulated=%" PRIu64 "ms count=%d)",
                         final_accumulated, s_filter.trigger_count);
            }
        }
        break;

    case RADAR_STATE_LOCKED_OUT:
        // 锁定状态：等待锁定时间结束
        if (time_reached_64(now, s_filter.lockout_until_ms)) {
            clear_filter_locked();
            ESP_LOGI(TAG, "[LOCKED→IDLE] lockout ended, radar resumed");
        }
        break;
    }

    xSemaphoreGive(s_state_mutex);

    // 状态转换日志
    if (old_state != s_filter.state) {
        ESP_LOGI(TAG, "State transition: %d → %d", old_state, s_filter.state);
    }
}

// ============================================================================
// 雷达任务 - 50ms轮询
// ============================================================================
static void radar_task(void *arg)
{
    (void)arg;

    bool last_in1 = false;
    bool last_in2 = false;
    int last_raw_in1 = -1;
    int last_raw_in2 = -1;
    uint64_t last_status_log_ms = 0;

    ESP_LOGI(TAG, "radar input task started: polling IO_IN1=GPIO%d IO_IN2=GPIO%d every %dms",
             PIN_IO_IN1, PIN_IO_IN2, RADAR_POLL_MS);

    while (1) {
        app_config_t cfg_copy;
        app_config_copy(&cfg_copy);

        board_inputs_t inputs = {0};
        if (board_hal_read_inputs(&inputs) == ESP_OK) {
            bool in1 = trigger_level(&cfg_copy.radar, inputs.io_in1);
            bool in2 = trigger_level(&cfg_copy.radar, inputs.io_in2);
            uint64_t now = time_ms_64();
            bool level_active = in1 || in2;

            // 输入变化日志
            if (inputs.io_in1 != last_raw_in1 || inputs.io_in2 != last_raw_in2 ||
                in1 != last_in1 || in2 != last_in2) {
                ESP_LOGI(TAG, "input change: IN1(GPIO%d)=%d active=%d IN2(GPIO%d)=%d active=%d",
                         PIN_IO_IN1, inputs.io_in1, in1,
                         PIN_IO_IN2, inputs.io_in2, in2);
                last_raw_in1 = inputs.io_in1;
                last_raw_in2 = inputs.io_in2;
            }

            // 执行过滤状态机
            radar_filter_step(&cfg_copy.radar, level_active, now);

            // 定期状态日志
            if (last_status_log_ms == 0 ||
                time_reached_64(now, last_status_log_ms + RADAR_STATUS_LOG_INTERVAL_MS)) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                ESP_LOGI(TAG, "status: state=%d IN1=%d IN2=%d enabled=%d trigger_count=%d accumulated_high=%" PRIu64 "ms",
                         s_filter.state, in1, in2, cfg_copy.radar.enabled,
                         s_filter.trigger_count, s_filter.accumulated_high_ms);
                xSemaphoreGive(s_state_mutex);
                last_status_log_ms = now;
            }

            last_in1 = in1;
            last_in2 = in2;
        }

        vTaskDelay(pdMS_TO_TICKS(RADAR_POLL_MS));
    }
}

// ============================================================================
// 启动雷达输入任务
// ============================================================================
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

    ESP_LOGI(TAG, "radar input module initialized successfully");
    return ESP_OK;
}
