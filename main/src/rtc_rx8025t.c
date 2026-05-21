#include "rtc_rx8025t.h"

#include <string.h>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "i2cdev.h"
#include "pin_map.h"

static const char *TAG = "rtc_rx8025t";

static i2c_dev_t s_rtc_dev = {0};
static bool s_ready = false;

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t bin_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool bcd_in_range(uint8_t raw, uint8_t min_value, uint8_t max_value)
{
    if ((raw & 0x0F) > 9 || ((raw >> 4) & 0x0F) > 9) {
        return false;
    }
    uint8_t value = bcd_to_bin(raw);
    return value >= min_value && value <= max_value;
}

static bool is_leap_year(int year)
{
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

static int days_in_month(int year, int month)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

static bool tm_date_is_valid(const struct tm *time_info)
{
    if (time_info == NULL ||
        time_info->tm_sec < 0 || time_info->tm_sec > 59 ||
        time_info->tm_min < 0 || time_info->tm_min > 59 ||
        time_info->tm_hour < 0 || time_info->tm_hour > 23 ||
        time_info->tm_mon < 0 || time_info->tm_mon > 11 ||
        time_info->tm_year < 100 || time_info->tm_year > 199) {
        return false;
    }

    int year = time_info->tm_year + 1900;
    int month = time_info->tm_mon + 1;
    return time_info->tm_mday >= 1 && time_info->tm_mday <= days_in_month(year, month);
}

static esp_err_t tm_to_utc_epoch(const struct tm *time_info, time_t *epoch)
{
    if (!tm_date_is_valid(time_info) || epoch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int year = time_info->tm_year + 1900;
    int month = time_info->tm_mon + 1;
    int64_t days = 0;

    for (int y = 1970; y < year; ++y) {
        days += is_leap_year(y) ? 366 : 365;
    }
    for (int m = 1; m < month; ++m) {
        days += days_in_month(year, m);
    }
    days += time_info->tm_mday - 1;

    *epoch = (time_t)((days * 86400) +
                      (time_info->tm_hour * 3600) +
                      (time_info->tm_min * 60) +
                      time_info->tm_sec);
    return ESP_OK;
}

static uint8_t weekday_to_rx8025t(int tm_wday)
{
    if (tm_wday < 0 || tm_wday > 6) {
        return 0x01;
    }
    return (uint8_t)(1U << tm_wday);
}

static int rx8025t_weekday_to_tm(uint8_t raw)
{
    for (int i = 0; i < 7; ++i) {
        if (raw & BIT(i)) {
            return i;
        }
    }
    return 0;
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_ready || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    I2C_DEV_TAKE_MUTEX(&s_rtc_dev);
    I2C_DEV_CHECK(&s_rtc_dev, i2c_dev_read_reg(&s_rtc_dev, reg, data, len));
    I2C_DEV_GIVE_MUTEX(&s_rtc_dev);
    return ESP_OK;
}

static esp_err_t write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!s_ready || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    I2C_DEV_TAKE_MUTEX(&s_rtc_dev);
    I2C_DEV_CHECK(&s_rtc_dev, i2c_dev_write_reg(&s_rtc_dev, reg, data, len));
    I2C_DEV_GIVE_MUTEX(&s_rtc_dev);
    return ESP_OK;
}

static void fill_flags(uint8_t raw, rtc_rx8025t_flags_t *flags)
{
    if (flags == NULL) {
        return;
    }

    flags->raw = raw;
    flags->voltage_low = (raw & RX8025T_FLAG_VLF) != 0;
    flags->voltage_detected = (raw & RX8025T_FLAG_VDET) != 0;
    flags->update_event = (raw & RX8025T_FLAG_UF) != 0;
    flags->timer_event = (raw & RX8025T_FLAG_TF) != 0;
    flags->alarm_event = (raw & RX8025T_FLAG_AF) != 0;
}

esp_err_t rtc_rx8025t_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    memset(&s_rtc_dev, 0, sizeof(s_rtc_dev));
    s_rtc_dev.port = I2C_PORT;
    s_rtc_dev.addr = RX8025T_I2C_ADDR;
    s_rtc_dev.cfg.sda_io_num = PIN_I2C1_SDA;
    s_rtc_dev.cfg.scl_io_num = PIN_I2C1_SCL;
    s_rtc_dev.cfg.master.clk_speed = I2C_FREQ_HZ;
    s_rtc_dev.cfg.clk_flags = 0;
    s_rtc_dev.cfg.scl_pullup_en = false;
    s_rtc_dev.cfg.sda_pullup_en = false;
    s_rtc_dev.timeout_ticks = I2CDEV_MAX_STRETCH_TIME;

    ESP_LOGI(TAG, "RX8025T-UB init: addr=0x%02X port=%d SDA=GPIO%d SCL=GPIO%d freq=%d Hz",
             RX8025T_I2C_ADDR, I2C_PORT, PIN_I2C1_SDA, PIN_I2C1_SCL, I2C_FREQ_HZ);
    esp_err_t err = i2c_dev_create_mutex(&s_rtc_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create I2C descriptor failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;

    uint8_t flag = 0;
    err = rtc_rx8025t_read_reg(RX8025T_REG_FLAG, &flag);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "probe flag register failed: %s", esp_err_to_name(err));
        s_ready = false;
        i2c_dev_delete_mutex(&s_rtc_dev);
        memset(&s_rtc_dev, 0, sizeof(s_rtc_dev));
        return err;
    }

    rtc_rx8025t_flags_t flags = {0};
    fill_flags(flag, &flags);
    ESP_LOGI(TAG, "RX8025T-UB ready: flag=0x%02X VLF=%d VDET=%d UF=%d TF=%d AF=%d",
             flag,
             flags.voltage_low,
             flags.voltage_detected,
             flags.update_event,
             flags.timer_event,
             flags.alarm_event);
    return ESP_OK;
}

esp_err_t rtc_rx8025t_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    s_ready = false;
    esp_err_t err = i2c_dev_delete_mutex(&s_rtc_dev);
    memset(&s_rtc_dev, 0, sizeof(s_rtc_dev));
    return err;
}

bool rtc_rx8025t_is_ready(void)
{
    return s_ready;
}

esp_err_t rtc_rx8025t_read_reg(uint8_t reg, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_regs(reg, value, 1);
}

esp_err_t rtc_rx8025t_write_reg(uint8_t reg, uint8_t value)
{
    return write_regs(reg, &value, 1);
}

esp_err_t rtc_rx8025t_read_time(struct tm *time_info, rtc_rx8025t_flags_t *flags)
{
    if (time_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t regs[7] = {0};
    ESP_RETURN_ON_ERROR(read_regs(RX8025T_REG_SECOND, regs, sizeof(regs)), TAG, "read time registers failed");

    uint8_t flag = 0;
    esp_err_t flag_err = rtc_rx8025t_read_reg(RX8025T_REG_FLAG, &flag);
    if (flag_err == ESP_OK) {
        fill_flags(flag, flags);
    }

    uint8_t second = regs[0] & 0x7F;
    uint8_t minute = regs[1] & 0x7F;
    uint8_t hour = regs[2] & 0x3F;
    uint8_t day = regs[4] & 0x3F;
    uint8_t month = regs[5] & 0x1F;
    uint8_t year = regs[6];

    if (!bcd_in_range(second, 0, 59) ||
        !bcd_in_range(minute, 0, 59) ||
        !bcd_in_range(hour, 0, 23) ||
        !bcd_in_range(day, 1, 31) ||
        !bcd_in_range(month, 1, 12) ||
        !bcd_in_range(year, 0, 99)) {
        ESP_LOGW(TAG, "invalid BCD time registers: %02X %02X %02X %02X %02X %02X %02X",
                 regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(time_info, 0, sizeof(*time_info));
    time_info->tm_sec = bcd_to_bin(second);
    time_info->tm_min = bcd_to_bin(minute);
    time_info->tm_hour = bcd_to_bin(hour);
    time_info->tm_mday = bcd_to_bin(day);
    time_info->tm_mon = bcd_to_bin(month) - 1;
    time_info->tm_year = bcd_to_bin(year) + 100;
    time_info->tm_wday = rx8025t_weekday_to_tm(regs[3] & 0x7F);
    time_info->tm_isdst = -1;
    return ESP_OK;
}

esp_err_t rtc_rx8025t_set_time(const struct tm *time_info)
{
    if (!tm_date_is_valid(time_info)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t control = 0;
    if (rtc_rx8025t_read_reg(RX8025T_REG_CONTROL, &control) == ESP_OK) {
        uint8_t reset = control | RX8025T_CONTR_RESET;
        ESP_RETURN_ON_ERROR(rtc_rx8025t_write_reg(RX8025T_REG_CONTROL, reset), TAG, "stop RTC counter failed");
    }

    uint8_t regs[7] = {
        bin_to_bcd((uint8_t)time_info->tm_sec),
        bin_to_bcd((uint8_t)time_info->tm_min),
        bin_to_bcd((uint8_t)time_info->tm_hour),
        weekday_to_rx8025t(time_info->tm_wday),
        bin_to_bcd((uint8_t)time_info->tm_mday),
        bin_to_bcd((uint8_t)(time_info->tm_mon + 1)),
        bin_to_bcd((uint8_t)(time_info->tm_year - 100)),
    };

    ESP_RETURN_ON_ERROR(write_regs(RX8025T_REG_SECOND, regs, sizeof(regs)), TAG, "write time registers failed");

    uint8_t flag = 0;
    if (rtc_rx8025t_read_reg(RX8025T_REG_FLAG, &flag) == ESP_OK) {
        flag &= (uint8_t)~(RX8025T_FLAG_VLF | RX8025T_FLAG_VDET | RX8025T_FLAG_UF | RX8025T_FLAG_TF | RX8025T_FLAG_AF);
        ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_rx8025t_write_reg(RX8025T_REG_FLAG, flag));
    }

    if (rtc_rx8025t_read_reg(RX8025T_REG_CONTROL, &control) == ESP_OK) {
        control &= (uint8_t)~RX8025T_CONTR_RESET;
        ESP_RETURN_ON_ERROR(rtc_rx8025t_write_reg(RX8025T_REG_CONTROL, control), TAG, "restart RTC counter failed");
    }

    ESP_LOGI(TAG, "RTC time written: %04d-%02d-%02d %02d:%02d:%02d",
             time_info->tm_year + 1900,
             time_info->tm_mon + 1,
             time_info->tm_mday,
             time_info->tm_hour,
             time_info->tm_min,
             time_info->tm_sec);
    return ESP_OK;
}

esp_err_t rtc_rx8025t_get_time(time_t *now)
{
    if (now == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm time_info = {0};
    rtc_rx8025t_flags_t flags = {0};
    ESP_RETURN_ON_ERROR(rtc_rx8025t_read_time(&time_info, &flags), TAG, "read RTC time failed");
    if (flags.voltage_low || flags.voltage_detected) {
        ESP_LOGW(TAG, "RTC voltage flag set: flag=0x%02X", flags.raw);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(tm_to_utc_epoch(&time_info, now), TAG, "convert RTC UTC time failed");
    return ESP_OK;
}

esp_err_t rtc_rx8025t_set_from_time(time_t now)
{
    if (now <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm time_info = {0};
    gmtime_r(&now, &time_info);
    return rtc_rx8025t_set_time(&time_info);
}

esp_err_t rtc_rx8025t_apply_to_system_time(void)
{
    time_t now = 0;
    ESP_RETURN_ON_ERROR(rtc_rx8025t_get_time(&now), TAG, "RTC time invalid");

    struct timeval tv = {
        .tv_sec = now,
        .tv_usec = 0,
    };
    ESP_RETURN_ON_ERROR(settimeofday(&tv, NULL), TAG, "settimeofday from RTC failed");

    struct tm time_info = {0};
    gmtime_r(&now, &time_info);
    ESP_LOGI(TAG, "System time restored from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
             time_info.tm_year + 1900,
             time_info.tm_mon + 1,
             time_info.tm_mday,
             time_info.tm_hour,
             time_info.tm_min,
             time_info.tm_sec);
    return ESP_OK;
}

esp_err_t rtc_rx8025t_save_system_time(void)
{
    time_t now = 0;
    time(&now);
    return rtc_rx8025t_set_from_time(now);
}

esp_err_t rtc_rx8025t_config_update_interrupt(bool enable, bool per_minute)
{
    uint8_t exten = 0;
    uint8_t control = 0;
    ESP_RETURN_ON_ERROR(rtc_rx8025t_read_reg(RX8025T_REG_EXTEN, &exten), TAG, "read EXTEN failed");
    ESP_RETURN_ON_ERROR(rtc_rx8025t_read_reg(RX8025T_REG_CONTROL, &control), TAG, "read CONTROL failed");

    exten &= (uint8_t)~RX8025T_EXTEN_TEST;
    if (per_minute) {
        exten |= RX8025T_EXTEN_USEL;
    } else {
        exten &= (uint8_t)~RX8025T_EXTEN_USEL;
    }

    if (enable) {
        control |= RX8025T_CONTR_UIE;
    } else {
        control &= (uint8_t)~RX8025T_CONTR_UIE;
    }

    ESP_RETURN_ON_ERROR(rtc_rx8025t_write_reg(RX8025T_REG_EXTEN, exten), TAG, "write EXTEN failed");
    ESP_RETURN_ON_ERROR(rtc_rx8025t_write_reg(RX8025T_REG_CONTROL, control), TAG, "write CONTROL failed");
    ESP_RETURN_ON_ERROR(rtc_rx8025t_clear_interrupt_flags(RX8025T_FLAG_UF), TAG, "clear UF failed");
    ESP_LOGI(TAG, "RTC update interrupt: %s, period=%s", enable ? "enabled" : "disabled", per_minute ? "minute" : "second");
    return ESP_OK;
}

esp_err_t rtc_rx8025t_clear_interrupt_flags(uint8_t mask)
{
    uint8_t flag = 0;
    ESP_RETURN_ON_ERROR(rtc_rx8025t_read_reg(RX8025T_REG_FLAG, &flag), TAG, "read FLAG failed");
    flag &= (uint8_t)~mask;
    return rtc_rx8025t_write_reg(RX8025T_REG_FLAG, flag);
}
