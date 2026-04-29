#include "modem_4g.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"

static const char *TAG = "modem_4g";

static esp_err_t set_level_checked(gpio_num_t pin, int level)
{
    esp_err_t err = gpio_set_level(pin, level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level(%d) failed: %s", pin, esp_err_to_name(err));
    }
    return err;
}

esp_err_t modem_4g_init(void)
{
    /*
     * board_hal has configured these GPIOs as outputs already.
     * Keep module in safe idle state by default.
     */
    ESP_RETURN_ON_ERROR(set_level_checked(PIN_4G_RESET, 0), TAG, "set 4G_RESET low failed");
    ESP_RETURN_ON_ERROR(set_level_checked(PIN_4G_PWRKEY, 0), TAG, "set 4G_PWRKEY low failed");
    ESP_LOGI(TAG, "4G control lines initialized (PWRKEY=%d RESET=%d)", PIN_4G_PWRKEY, PIN_4G_RESET);
    return ESP_OK;
}

esp_err_t modem_4g_power_on(void)
{
    /* Typical modem power-key pulse sequence. */
    ESP_LOGI(TAG, "4G power-on pulse start");
    ESP_RETURN_ON_ERROR(set_level_checked(PIN_4G_PWRKEY, 1), TAG, "set PWRKEY high failed");
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP_RETURN_ON_ERROR(set_level_checked(PIN_4G_PWRKEY, 0), TAG, "set PWRKEY low failed");
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "4G power-on pulse done");
    return ESP_OK;
}

esp_err_t modem_4g_power_off(void)
{
    /* Typical graceful power-off pulse (module dependent, kept conservative). */
    ESP_LOGI(TAG, "4G power-off pulse start");
    ESP_RETURN_ON_ERROR(set_level_checked(PIN_4G_PWRKEY, 1), TAG, "set PWRKEY high failed");
    vTaskDelay(pdMS_TO_TICKS(2500));
    ESP_RETURN_ON_ERROR(set_level_checked(PIN_4G_PWRKEY, 0), TAG, "set PWRKEY low failed");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "4G power-off pulse done");
    return ESP_OK;
}

esp_err_t modem_4g_hard_reset(void)
{
    ESP_LOGI(TAG, "4G hard reset pulse start");
    ESP_RETURN_ON_ERROR(set_level_checked(PIN_4G_RESET, 1), TAG, "set RESET high failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(set_level_checked(PIN_4G_RESET, 0), TAG, "set RESET low failed");
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "4G hard reset pulse done");
    return ESP_OK;
}
