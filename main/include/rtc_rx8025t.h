#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

/* RX-8025T-UB 寄存器表；该芯片与 RX-8025 SA/NB 不完全相同。 */
#define RX8025T_REG_SECOND               0x00
#define RX8025T_REG_MINUTE               0x01
#define RX8025T_REG_HOUR                 0x02
#define RX8025T_REG_WEEKDAY              0x03
#define RX8025T_REG_DAY                  0x04
#define RX8025T_REG_MONTH                0x05
#define RX8025T_REG_YEAR                 0x06
#define RX8025T_REG_RAM                  0x07
#define RX8025T_REG_ALARM_MIN            0x08
#define RX8025T_REG_ALARM_HOUR           0x09
#define RX8025T_REG_ALARM_W_OR_D         0x0A
#define RX8025T_REG_TIMER_COUNTER0       0x0B
#define RX8025T_REG_TIMER_COUNTER1       0x0C
#define RX8025T_REG_EXTEN                0x0D
#define RX8025T_REG_FLAG                 0x0E
#define RX8025T_REG_CONTROL              0x0F

#define RX8025T_EXTEN_TEST               0x80
#define RX8025T_EXTEN_WADA               0x40
#define RX8025T_EXTEN_USEL               0x20
#define RX8025T_EXTEN_TE                 0x10
#define RX8025T_EXTEN_FSEL1              0x08
#define RX8025T_EXTEN_FSEL0              0x04
#define RX8025T_EXTEN_TSEL1              0x02
#define RX8025T_EXTEN_TSEL0              0x01

#define RX8025T_FLAG_UF                  0x20
#define RX8025T_FLAG_TF                  0x10
#define RX8025T_FLAG_AF                  0x08
#define RX8025T_FLAG_VLF                 0x02
#define RX8025T_FLAG_VDET                0x01

#define RX8025T_CONTR_CSEL1              0x80
#define RX8025T_CONTR_CSEL0              0x40
#define RX8025T_CONTR_UIE                0x20
#define RX8025T_CONTR_TIE                0x10
#define RX8025T_CONTR_AIE                0x08
#define RX8025T_CONTR_RESET              0x01

typedef struct {
    bool voltage_low;
    bool voltage_detected;
    bool update_event;
    bool timer_event;
    bool alarm_event;
    uint8_t raw;
} rtc_rx8025t_flags_t;

esp_err_t rtc_rx8025t_init(void);
esp_err_t rtc_rx8025t_deinit(void);
bool rtc_rx8025t_is_ready(void);

esp_err_t rtc_rx8025t_read_reg(uint8_t reg, uint8_t *value);
esp_err_t rtc_rx8025t_write_reg(uint8_t reg, uint8_t value);
esp_err_t rtc_rx8025t_read_time(struct tm *time_info, rtc_rx8025t_flags_t *flags);
esp_err_t rtc_rx8025t_set_time(const struct tm *time_info);
esp_err_t rtc_rx8025t_get_time(time_t *now);
esp_err_t rtc_rx8025t_set_from_time(time_t now);
esp_err_t rtc_rx8025t_apply_to_system_time(void);
esp_err_t rtc_rx8025t_save_system_time(void);
esp_err_t rtc_rx8025t_config_update_interrupt(bool enable, bool per_minute);
esp_err_t rtc_rx8025t_clear_interrupt_flags(uint8_t mask);
