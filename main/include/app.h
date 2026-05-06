#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool board_ready;
    bool nfc_ready;
    bool sd_ready;
    bool eth_ready;
    bool modem_started;
    int sd_det_at_init;
} app_devices_t;

esp_err_t app_work_start(const app_devices_t *devices);
