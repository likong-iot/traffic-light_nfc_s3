#include "app.h"

#include "board_hal.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "modem_4g.h"
#include "net_eth.h"
#include "nfc_pn532.h"
#include "nvs_flash.h"
#include "pin_map.h"
#include "storage_sd.h"

static const char *TAG = "app";

void app_start(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Traffic Light NFC System — peripheral startup");
    ESP_LOGI(TAG, "================================================");

    /* Step 1: NVS flash */
    ESP_LOGI(TAG, "[1/7] NVS flash init");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "[1/7] NVS requires erase (reason: %s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "[1/7] NVS ready");

    /* Step 2: TCP/IP stack + default event loop */
    ESP_LOGI(TAG, "[2/7] TCP/IP stack init (esp_netif_init)");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG, "[2/7] Default event loop init (esp_event_loop_create_default)");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_LOGI(TAG, "[2/7] TCP/IP stack + event loop ready");

    /* Step 3: Board GPIO + I2C + PCF8574 */
    ESP_LOGI(TAG, "[3/7] Board HAL init (GPIO outputs/inputs + I2C + PCF8574)");
    ESP_ERROR_CHECK(board_hal_init());
    board_hal_log_map();
    ESP_LOGI(TAG, "[3/7] Board HAL ready");

    /* Step 4: PN532 NFC reader */
    ESP_LOGI(TAG, "[4/7] PN532 NFC reader init (HSU/UART)");
    bool nfc_ok = (nfc_pn532_init() == ESP_OK);
    if (!nfc_ok) {
        ESP_LOGW(TAG, "[4/7] PN532 init failed — NFC unavailable");
    } else {
        ESP_LOGI(TAG, "[4/7] PN532 ready");
    }

    /* Step 5: SD card (SDMMC 1-bit) */
    ESP_LOGI(TAG, "[5/7] SD card init check");
    int sd_det = gpio_get_level(PIN_SD_DET);
    ESP_LOGI(TAG, "[5/7] SD_DET=GPIO%d  level=%d  (%s)",
             PIN_SD_DET, sd_det, sd_det == 0 ? "card present" : "no card");
    err = storage_sd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[5/7] SD init failed (%s) — storage unavailable", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[5/7] SD ready at '%s'", storage_sd_mount_point());
    }

    /* Step 6: W5500 Ethernet (SPI2, direct driver) */
    ESP_LOGI(TAG, "[6/7] W5500 Ethernet init (SPI2, direct driver — no ethernet_init component)");
    err = net_eth_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[6/7] W5500 init failed (%s) — ethernet unavailable", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[6/7] W5500 driver started (waiting for link on GPIO%d)", PIN_W5500_INT);
    }

    /* Step 7: AIR780ER 4G modem (USB OTG, non-blocking background task) */
    ESP_LOGI(TAG, "[7/7] AIR780ER 4G modem init (USB OTG, background task)");
    err = modem_4g_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[7/7] 4G modem task start failed (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[7/7] 4G modem task started — connecting in background");
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Peripheral init complete");
    ESP_LOGI(TAG, "  NFC=%s  SD=%s  ETH=%s  4G=connecting",
             nfc_ok ? "ready" : "unavail",
             storage_sd_is_mounted() ? "mounted" : (sd_det == 0 ? "failed" : "absent"),
             net_eth_is_connected() ? "linked" : "waiting");
    ESP_LOGI(TAG, "================================================");
}
