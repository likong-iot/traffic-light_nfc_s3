#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    int key1;
    int key2;
    int io_in1;
    int io_in2;
    int rtc_int;
    int sd_det;
    int eth_int;
} board_inputs_t;

esp_err_t board_hal_init(void);
void board_hal_log_map(void);
esp_err_t board_hal_read_inputs(board_inputs_t *inputs);
esp_err_t board_hal_set_ledb(bool on);
esp_err_t board_hal_set_led1(bool on);
esp_err_t board_hal_set_led2(bool on);
esp_err_t board_hal_pulse_opto12(int pulse_count, int active_ms, int inactive_gap_ms);
esp_err_t board_hal_set_relay(int relay_num, bool closed);
esp_err_t board_hal_set_all_relays(bool closed);
esp_err_t board_hal_pulse_relay(int relay_num, int pulse_count, int active_ms, int inactive_gap_ms);
esp_err_t board_hal_pulse_io_out1(int pulse_count, int high_ms, int low_gap_ms);
