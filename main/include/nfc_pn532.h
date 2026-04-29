#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t uid[10];
    uint8_t uid_length;
    uint8_t data[16];
    uint8_t data_length;
    uint8_t command_value;
    const char *source;
} nfc_card_command_t;

esp_err_t nfc_pn532_init(void);
esp_err_t nfc_pn532_start_card_scan(void);
esp_err_t nfc_pn532_read_card_command(nfc_card_command_t *card);
void nfc_pn532_deinit(void);
