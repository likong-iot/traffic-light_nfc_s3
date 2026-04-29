#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t net_eth_init(void);
esp_err_t net_eth_start(void);
esp_err_t net_eth_stop(void);
bool net_eth_is_started(void);
