#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

enum {
    APP_CONFIG_VERSION = 4,
    APP_CONFIG_MAX_NFC_RULES = 16,
    APP_CONFIG_CARD_CLASS_COUNT = 6,
};

typedef enum {
    APP_CONFIG_SOURCE_DEFAULT = 0,
    APP_CONFIG_SOURCE_NVS,
    APP_CONFIG_SOURCE_SD,
} app_config_source_t;

typedef struct {
    char data[3];
    char name[32];
    int opto12_pulses;
} app_nfc_rule_t;

// ============================================================================
// 雷达配置 - 修复日期: 2026-06-02
// 新增: high_level_threshold_ms, trigger_count_threshold
// 用途: 实现累计高电平时间和触发次数统计的干扰检测
// ============================================================================
typedef struct {
    bool enabled;                    // 雷达功能开关
    int active_level;                // 触发电平 (0=低电平, 1=高电平)

    // 触发确认
    int trigger_delay_ms;            // 延时确认时间（默认5000ms=5秒）

    // 窗口与去重
    int cycle_window_ms;             // 窗口时间（默认20000ms=20秒）

    // 干扰检测条件A：累计高电平时间
    int high_level_threshold_ms;     // 新增：累计高电平阈值（默认15000ms=15秒）

    // 干扰检测条件B：触发次数
    int trigger_count_threshold;     // 新增：触发次数阈值（默认5次）

    // 干扰锁定
    int interference_cycles;         // 连续干扰周期阈值（默认3次）
    int lockout_ms;                  // 锁定时长（默认300000ms=10分钟）

    // 输出动作
    int opto12_pulses;               // 光耦脉冲数
} app_radar_config_t;

typedef struct {
    int class1_led1_hold_ms;
    int class2_led1_hold_ms;
    int class3_led2_hold_ms;
} app_timing_config_t;

typedef struct {
    int relay1_start_min;
    int relay1_end_min;
    int relay2_start_min;
    int relay2_end_min;
} app_schedule_config_t;

typedef struct {
    char ssid[33];
    char password[64];
} app_ap_config_t;

typedef struct {
    int version;
    app_timing_config_t timing;
    app_schedule_config_t schedule;
    app_ap_config_t ap;
    app_nfc_rule_t nfc_rules[APP_CONFIG_MAX_NFC_RULES];
    size_t nfc_rule_count;
    app_radar_config_t radar;
    bool log_enabled;
    app_config_source_t source;
} app_config_t;

esp_err_t app_config_init(void);
const app_config_t *app_config_get(void);
void app_config_copy(app_config_t *out);
esp_err_t app_config_save(const app_config_t *cfg);
esp_err_t app_config_find_nfc_action(uint8_t data0, uint8_t data1, app_nfc_rule_t *rule, uint8_t *class_id);
const char *app_config_source_name(app_config_source_t source);
void app_config_build_default_ap_name(char *out, size_t out_len);
