#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    int bytes_downloaded;
    int bytes_total;
    char message[128];
} ota_status_t;

esp_err_t ota_update_init(void);
esp_err_t ota_update_start(const char *url);
void ota_update_get_status(ota_status_t *out);
const char *ota_state_name(ota_state_t state);
const char *ota_running_version(void);
