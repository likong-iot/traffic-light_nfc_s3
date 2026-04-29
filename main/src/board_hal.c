#include "board_hal.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2cdev.h"
#include "pcf8574.h"
#include "pin_map.h"

static const char *TAG = "board_hal";
static i2c_dev_t s_pcf_dev = {0};
static bool s_pcf_ready = false;

static esp_err_t config_output(gpio_num_t pin, int level)
{
    ESP_LOGI(TAG, "    GPIO%d -> OUTPUT  pull=none  init_level=%d", pin, level);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "    gpio_config GPIO%d failed: %s", pin, esp_err_to_name(err));
        return err;
    }
    err = gpio_set_level(pin, level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "    gpio_set_level GPIO%d=%d failed: %s", pin, level, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "    GPIO%d OK (level=%d)", pin, level);
    return ESP_OK;
}

static esp_err_t config_input(gpio_num_t pin, bool pullup)
{
    ESP_LOGI(TAG, "    GPIO%d -> INPUT  pull_up=%s", pin, pullup ? "yes" : "no");
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pullup ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "    gpio_config GPIO%d failed: %s", pin, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "    GPIO%d OK (read=%d)", pin, gpio_get_level(pin));
    return err;
}

static esp_err_t init_i2c_and_pcf8574(void)
{
    ESP_LOGI(TAG, "  [I2C 1/4] i2cdev global init (manages port=%d SDA=GPIO%d SCL=GPIO%d freq=%d Hz)",
             I2C_PORT, PIN_I2C1_SDA, PIN_I2C1_SCL, I2C_FREQ_HZ);
    esp_err_t err = i2cdev_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  [I2C 1/4] i2cdev_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  [I2C 1/4] i2cdev global init OK");

    ESP_LOGI(TAG, "  [I2C 2/4] PCF8574 descriptor: addr=0x%02X port=%d SDA=GPIO%d SCL=GPIO%d",
             PCF8574_ADDR, I2C_PORT, PIN_I2C1_SDA, PIN_I2C1_SCL);
    err = pcf8574_init_desc(&s_pcf_dev, PCF8574_ADDR, I2C_PORT, PIN_I2C1_SDA, PIN_I2C1_SCL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  [I2C 2/4] pcf8574_init_desc @0x%02X failed: %s",
                 PCF8574_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  [I2C 2/4] PCF8574 descriptor created OK");

    ESP_LOGI(TAG, "  [I2C 3/4] Setting bus clock speed: %d Hz", I2C_FREQ_HZ);
    s_pcf_dev.cfg.master.clk_speed = I2C_FREQ_HZ;
    ESP_LOGI(TAG, "  [I2C 3/4] Clock speed set OK");

    ESP_LOGI(TAG, "  [I2C 4/4] PCF8574 port_write(0xFF) — all outputs HIGH (relays off, safe default)");
    err = pcf8574_port_write(&s_pcf_dev, 0xFF);
    if (err != ESP_OK) {
        pcf8574_free_desc(&s_pcf_dev);
        ESP_LOGE(TAG, "  [I2C 4/4] pcf8574_port_write failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  [I2C 4/4] PCF8574 port write OK");

    s_pcf_ready = true;
    ESP_LOGI(TAG, "  PCF8574 @0x%02X ready (SDA=GPIO%d SCL=GPIO%d freq=%d Hz)",
             PCF8574_ADDR, PIN_I2C1_SDA, PIN_I2C1_SCL, I2C_FREQ_HZ);
    return ESP_OK;
}

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "=== board_hal_init start ===");

    /* Step 1: Status LED */
    ESP_LOGI(TAG, "[1/5] Status LED: GPIO%d  active_level=%d  init=OFF",
             PIN_LED1, LED_ACTIVE_LEVEL);
    ESP_RETURN_ON_ERROR(config_output(PIN_LED1, !LED_ACTIVE_LEVEL), TAG, "LED1 failed");
    ESP_LOGI(TAG, "[1/5] LED1 OK");

    /* Step 2: Relay opto-drive outputs */
    ESP_LOGI(TAG, "[2/5] Relay outputs: IO_OUT1=GPIO%d  IO_OUT2=GPIO%d  IO_OUT3=GPIO%d  IO_OUT4=GPIO%d  (default HIGH=off)",
             PIN_IO_OUT1, PIN_IO_OUT2, PIN_IO_OUT3, PIN_IO_OUT4);
    ESP_RETURN_ON_ERROR(config_output(PIN_IO_OUT1, 1), TAG, "IO_OUT1 failed");
    ESP_RETURN_ON_ERROR(config_output(PIN_IO_OUT2, 1), TAG, "IO_OUT2 failed");
    ESP_RETURN_ON_ERROR(config_output(PIN_IO_OUT3, 1), TAG, "IO_OUT3 failed");
    ESP_RETURN_ON_ERROR(config_output(PIN_IO_OUT4, 1), TAG, "IO_OUT4 failed");
    ESP_LOGI(TAG, "[2/5] Relay outputs OK");

    /* Step 3: Key inputs */
    ESP_LOGI(TAG, "[3/5] Key inputs: KEY1=GPIO%d  KEY2=GPIO%d  (pull_up, active_low)",
             PIN_KEY1, PIN_KEY2);
    ESP_RETURN_ON_ERROR(config_input(PIN_KEY1, true), TAG, "KEY1 failed");
    ESP_RETURN_ON_ERROR(config_input(PIN_KEY2, true), TAG, "KEY2 failed");
    ESP_LOGI(TAG, "[3/5] KEY1=%d  KEY2=%d  OK",
             gpio_get_level(PIN_KEY1), gpio_get_level(PIN_KEY2));

    /* Step 4: SD card detect. GPIO1 is detect/power-enable hardware, not SD DAT3. */
    ESP_LOGI(TAG, "[4/5] SD card detect: GPIO%d  (pull_up, active_low=card present)", PIN_SD_DET);
    ESP_RETURN_ON_ERROR(config_input(PIN_SD_DET, true), TAG, "SD_DET failed");
    int sd_det = gpio_get_level(PIN_SD_DET);
    ESP_LOGI(TAG, "[4/5] SD_DET=GPIO%d  level=%d  (%s)",
             PIN_SD_DET, sd_det, sd_det == 0 ? "card present" : "no card");

    /* Step 5: I2C bus + PCF8574 IO expander */
    ESP_LOGI(TAG, "[5/5] I2C bus (port=%d SDA=GPIO%d SCL=GPIO%d %d Hz) + PCF8574 @0x%02X",
             I2C_PORT, PIN_I2C1_SDA, PIN_I2C1_SCL, I2C_FREQ_HZ, PCF8574_ADDR);
    esp_err_t err = init_i2c_and_pcf8574();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[5/5] PCF8574 init failed (%s), continuing without expander",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[5/5] I2C + PCF8574 OK");
    }

    ESP_LOGI(TAG, "=== board_hal_init done (KEY1=%d KEY2=%d SD_DET=%d PCF_OK=%d) ===",
             gpio_get_level(PIN_KEY1),
             gpio_get_level(PIN_KEY2),
             gpio_get_level(PIN_SD_DET),
             s_pcf_ready);
    return ESP_OK;
}

void board_hal_log_map(void)
{
    ESP_LOGI(TAG, "--- Pin map ---");
    ESP_LOGI(TAG, "  LED1=GPIO%d (active=%d)", PIN_LED1, LED_ACTIVE_LEVEL);
    ESP_LOGI(TAG, "  KEY1=GPIO%d  KEY2=GPIO%d", PIN_KEY1, PIN_KEY2);
    ESP_LOGI(TAG, "  IO_OUT1=GPIO%d  IO_OUT2=GPIO%d  IO_OUT3=GPIO%d  IO_OUT4=GPIO%d",
             PIN_IO_OUT1, PIN_IO_OUT2, PIN_IO_OUT3, PIN_IO_OUT4);
    ESP_LOGI(TAG, "  I2C SDA=GPIO%d  SCL=GPIO%d  PCF8574=0x%02X",
             PIN_I2C1_SDA, PIN_I2C1_SCL, PCF8574_ADDR);
    ESP_LOGI(TAG, "  NFC UART%d  TX=GPIO%d  RX=GPIO%d  baud=%d",
             NFC_UART_PORT, PIN_NFC_TX, PIN_NFC_RX, NFC_UART_BAUD);
    ESP_LOGI(TAG, "  4G PWRKEY=GPIO%d  RESET=GPIO%d  USB_DP=GPIO%d  USB_DN=GPIO%d",
             PIN_4G_PWRKEY, PIN_4G_RESET, PIN_4G_USB_DP, PIN_4G_USB_DN);
    ESP_LOGI(TAG, "  W5500 MOSI=GPIO%d  MISO=GPIO%d  CLK=GPIO%d  CS=GPIO%d  INT=GPIO%d  RST=GPIO%d",
             PIN_W5500_MOSI, PIN_W5500_MISO, PIN_W5500_SCLK,
             PIN_W5500_CS, PIN_W5500_INT, PIN_W5500_RST);
    ESP_LOGI(TAG, "  SD DET=GPIO%d  D0=GPIO%d  CMD=GPIO%d  CLK=GPIO%d",
             PIN_SD_DET, PIN_SD_D0, PIN_SD_CMD, PIN_SD_CLK);
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

esp_err_t board_hal_pulse_io_out1(int pulse_count, int high_ms, int low_gap_ms)
{
    if (pulse_count <= 0 || high_ms <= 0 || low_gap_ms < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "IO_OUT1 pulse sequence: count=%d high=%d ms gap_low=%d ms",
             pulse_count, high_ms, low_gap_ms);

    esp_err_t err = gpio_set_level(PIN_IO_OUT1, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO_OUT1 set idle LOW failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    for (int i = 0; i < pulse_count; ++i) {
        ESP_LOGI(TAG, "IO_OUT1 pulse %d/%d: HIGH %d ms", i + 1, pulse_count, high_ms);
        err = gpio_set_level(PIN_IO_OUT1, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IO_OUT1 set HIGH failed: %s", esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(high_ms));

        err = gpio_set_level(PIN_IO_OUT1, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IO_OUT1 set LOW failed: %s", esp_err_to_name(err));
            return err;
        }

        if (i + 1 < pulse_count && low_gap_ms > 0) {
            ESP_LOGI(TAG, "IO_OUT1 inter-pulse LOW gap %d ms", low_gap_ms);
            vTaskDelay(pdMS_TO_TICKS(low_gap_ms));
        }
    }

    ESP_LOGI(TAG, "IO_OUT1 pulse sequence done, final level LOW");
    return ESP_OK;
}
