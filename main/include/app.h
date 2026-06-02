#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_CONTROL_EVENT_RADAR_READY = 1,
} app_control_event_type_t;

typedef struct {
    app_control_event_type_t type;
    uint64_t timestamp_ms;  // 修复: 改用64位防止49天溢出
} app_control_event_t;

typedef struct {
    bool board_ready;
    bool nfc_ready;
    bool sd_ready;
    bool eth_ready;
    bool modem_started;
    int sd_det_at_init;
} app_devices_t;

esp_err_t app_work_start(const app_devices_t *devices);
esp_err_t app_post_control_event(const app_control_event_t *event);
