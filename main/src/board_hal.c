#include "board_hal.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "i2cdev.h"
#include "pcf8574.h"
#include "pin_map.h"

static const char *TAG = "board_hal";
static i2c_dev_t s_pcf_dev = {0};

static esp_err_t config_gpio_output(gpio_num_t pin, int level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(pin, level);
}

static esp_err_t config_gpio_input(gpio_num_t pin, bool pullup)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pullup ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static esp_err_t init_i2c_and_pcf8574(void)
{
    esp_err_t err = i2cdev_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2cdev_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = pcf8574_init_desc(&s_pcf_dev, PCF8574_ADDR, I2C_PORT, PIN_I2C1_SDA, PIN_I2C1_SCL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcf8574_init_desc failed: %s", esp_err_to_name(err));
        return err;
    }
    s_pcf_dev.cfg.master.clk_speed = I2C_FREQ_HZ;

    /*
     * Safe default for relay chain:
     * keep PCF outputs high so downstream ULN stages stay in OFF state.
     */
    err = pcf8574_port_write(&s_pcf_dev, 0xFF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcf8574_port_write failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "PCF8574 ready @0x%02X (SDA=%d SCL=%d)", PCF8574_ADDR, PIN_I2C1_SDA, PIN_I2C1_SCL);
    return ESP_OK;
}

static esp_err_t init_nfc_uart(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = NFC_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(NFC_UART_PORT, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install(UART1) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(NFC_UART_PORT, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config(UART1) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(NFC_UART_PORT, PIN_NFC_TX, PIN_NFC_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin(UART1) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "NFC UART ready (TX=%d RX=%d %d baud)", PIN_NFC_TX, PIN_NFC_RX, NFC_UART_BAUD);
    return ESP_OK;
}

esp_err_t board_hal_init(void)
{
    /* Outputs: set safe defaults first */
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_LED1, !LED_ACTIVE_LEVEL), TAG, "init LED1 failed");
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_IO_OUT1, 1), TAG, "init IO_OUT1 failed");
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_IO_OUT2, 1), TAG, "init IO_OUT2 failed");
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_IO_OUT3, 1), TAG, "init IO_OUT3 failed");
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_IO_OUT4, 1), TAG, "init IO_OUT4 failed");
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_4G_PWRKEY, 0), TAG, "init 4G_PWRKEY failed");
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_4G_RESET, 0), TAG, "init 4G_RESET failed");
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_W5500_CS, 1), TAG, "init W5500_CS failed");
    ESP_RETURN_ON_ERROR(config_gpio_output(PIN_W5500_RST, 1), TAG, "init W5500_RST failed");

    /* Inputs */
    ESP_RETURN_ON_ERROR(config_gpio_input(PIN_KEY1, true), TAG, "init KEY1 failed");
    ESP_RETURN_ON_ERROR(config_gpio_input(PIN_KEY2, true), TAG, "init KEY2 failed");
    ESP_RETURN_ON_ERROR(config_gpio_input(PIN_W5500_INT, true), TAG, "init W5500_INT failed");
    ESP_RETURN_ON_ERROR(config_gpio_input(PIN_SD_DET, true), TAG, "init SD_DET failed");

    ESP_RETURN_ON_ERROR(init_i2c_and_pcf8574(), TAG, "init PCF8574 failed");
    ESP_RETURN_ON_ERROR(init_nfc_uart(), TAG, "init NFC UART failed");
    return ESP_OK;
}

void board_hal_log_map(void)
{
    ESP_LOGI(TAG, "GPIO map:");
    ESP_LOGI(TAG, "  LED1=%d (active=%d)", PIN_LED1, LED_ACTIVE_LEVEL);
    ESP_LOGI(TAG, "  KEY1=%d KEY2=%d", PIN_KEY1, PIN_KEY2);
    ESP_LOGI(TAG, "  IO_OUT1=%d IO_OUT2=%d IO_OUT3=%d IO_OUT4=%d", PIN_IO_OUT1, PIN_IO_OUT2, PIN_IO_OUT3, PIN_IO_OUT4);
    ESP_LOGI(TAG, "  I2C1_SDA=%d I2C1_SCL=%d", PIN_I2C1_SDA, PIN_I2C1_SCL);
    ESP_LOGI(TAG, "  NFC_TX=%d NFC_RX=%d", PIN_NFC_TX, PIN_NFC_RX);
    ESP_LOGI(TAG, "  4G_PWRKEY=%d 4G_RESET=%d", PIN_4G_PWRKEY, PIN_4G_RESET);
    ESP_LOGI(TAG, "  4G_USB_DP=%d 4G_USB_DN=%d", PIN_4G_USB_DP, PIN_4G_USB_DN);
    ESP_LOGI(TAG, "  W5500 INT=%d MOSI=%d MISO=%d SCLK=%d CS=%d RST=%d",
             PIN_W5500_INT, PIN_W5500_MOSI, PIN_W5500_MISO, PIN_W5500_SCLK, PIN_W5500_CS, PIN_W5500_RST);
    ESP_LOGI(TAG, "  SD DET=%d D0=%d CMD=%d CLK=%d", PIN_SD_DET, PIN_SD_D0, PIN_SD_CMD, PIN_SD_CLK);
}

esp_err_t board_hal_read_inputs(board_inputs_t *inputs)
{
    if (inputs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    inputs->key1 = gpio_get_level(PIN_KEY1);
    inputs->key2 = gpio_get_level(PIN_KEY2);
    inputs->sd_det = gpio_get_level(PIN_SD_DET);
    inputs->eth_int = gpio_get_level(PIN_W5500_INT);
    return ESP_OK;
}
