#pragma once

#include "esp_err.h"

esp_err_t modem_4g_init(void);
esp_err_t modem_4g_power_on(void);
esp_err_t modem_4g_power_off(void);
esp_err_t modem_4g_hard_reset(void);
