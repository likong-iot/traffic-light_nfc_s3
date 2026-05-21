#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

esp_err_t time_sync_start(void);
bool time_sync_is_synced(void);
bool time_sync_is_network_synced(void);
bool time_sync_is_rtc_synced(void);
esp_err_t time_sync_get_time(time_t *now);
