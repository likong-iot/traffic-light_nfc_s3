#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t net_eth_init(void);
bool net_eth_is_connected(void);
