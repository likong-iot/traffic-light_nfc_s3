#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Start the 4G modem initialisation in a background task (non-blocking). */
esp_err_t modem_4g_init(void);

/* Returns true once PPP is up and an IP address has been obtained. */
bool modem_4g_is_connected(void);
