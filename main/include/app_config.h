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

typedef struct {
    bool enabled;
    int active_level;
    int trigger_delay_ms;
    int cycle_window_ms;
    int interference_cycles;
    int lockout_ms;
    int opto12_pulses;
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
