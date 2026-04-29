#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t storage_sd_init(void);
esp_err_t storage_sd_probe_rw(void);
esp_err_t storage_sd_deinit(void);
bool storage_sd_is_mounted(void);
const char *storage_sd_mount_point(void);
