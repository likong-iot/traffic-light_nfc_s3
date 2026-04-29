#pragma once

#include "esp_err.h"

typedef struct {
    int key1;
    int key2;
    int sd_det;
    int eth_int;
} board_inputs_t;

esp_err_t board_hal_init(void);
void board_hal_log_map(void);
esp_err_t board_hal_read_inputs(board_inputs_t *inputs);
esp_err_t board_hal_pulse_io_out1(int pulse_count, int high_ms, int low_gap_ms);
