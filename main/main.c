#include "app.h"
#include "app_config.h"

#include <string.h>

#include "board_hal.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "modem_4g.h"
#include "net_eth.h"
#include "nfc_pn532.h"
#include "nvs_flash.h"
#include "peripheral_test.h"
#include "radar_input.h"
#include "pin_map.h"
#include "storage_sd.h"
#include "time_sync.h"
#include "web_config.h"

static const char *TAG = "main";

/*
 * Bring-up switch: keep the AIR780E modem code in the project, but do not
 * start its background state machine while PN532/SD/IO tests are being run.
 */
#ifndef APP_ENABLE_4G_MODEM
#define APP_ENABLE_4G_MODEM 1
#endif

static app_devices_t s_devices = {0};

static esp_err_t platform_init(void)
{
    ESP_LOGI(TAG, "[main init 1/3] Platform init: NVS flash");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires erase (reason: %s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init failed");
    ESP_LOGI(TAG, "NVS ready");

    ESP_LOGI(TAG, "[main init 2/3] Platform init: TCP/IP stack + default event loop");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "esp_event_loop_create_default failed");
    }
    ESP_LOGI(TAG, "TCP/IP stack + event loop ready");
    return ESP_OK;
}

static esp_err_t devices_init(app_devices_t *devices)
{
    if (devices == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(devices, 0, sizeof(*devices));
    devices->sd_det_at_init = 1;

    ESP_LOGI(TAG, "[main init 3/3] Device init bundle start");

    ESP_LOGI(TAG, "[devices 1/5] Board HAL init (GPIO outputs/inputs + I2C + PCF8574)");
    ESP_RETURN_ON_ERROR(board_hal_init(), TAG, "board_hal_init failed");
    board_hal_log_map();
    devices->board_ready = true;
    ESP_LOGI(TAG, "[devices 1/5] Board HAL ready");

    ESP_LOGI(TAG, "[devices 2/5] PN532 NFC reader init (HSU/UART)");
    esp_err_t err = nfc_pn532_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[devices 2/5] PN532 init failed (%s) - NFC unavailable",
                 esp_err_to_name(err));
    } else {
        devices->nfc_ready = true;
        ESP_LOGI(TAG, "[devices 2/5] PN532 ready");
    }

    ESP_LOGI(TAG, "[devices 3/5] SD card init check");
    int sd_det = gpio_get_level(PIN_SD_DET);
    devices->sd_det_at_init = sd_det;
    ESP_LOGI(TAG, "[devices 3/5] SD_DET=GPIO%d  level=%d  (%s)",
             PIN_SD_DET, sd_det, sd_det == 0 ? "card present" : "no card");
    err = storage_sd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[devices 3/5] SD init failed (%s) - storage unavailable",
                 esp_err_to_name(err));
    } else {
        devices->sd_ready = true;
        ESP_LOGI(TAG, "[devices 3/5] SD ready at '%s'", storage_sd_mount_point());
    }

    ESP_LOGI(TAG, "[devices 4/5] W5500 Ethernet init (SPI2, direct driver - no ethernet_init component)");
    err = net_eth_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[devices 4/5] W5500 init failed (%s) - ethernet unavailable",
                 esp_err_to_name(err));
    } else {
        devices->eth_ready = true;
        ESP_LOGI(TAG, "[devices 4/5] W5500 driver started (waiting for link on GPIO%d)",
                 PIN_W5500_INT);
    }

#if APP_ENABLE_4G_MODEM
    ESP_LOGI(TAG, "[devices 5/5] AIR780ER 4G modem init (USB OTG, background task)");
    err = modem_4g_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[devices 5/5] 4G modem task start failed (%s)", esp_err_to_name(err));
    } else {
        devices->modem_started = true;
        ESP_LOGI(TAG, "[devices 5/5] 4G modem task started - connecting in background");
    }
#else
    ESP_LOGW(TAG, "[devices 5/5] AIR780ER 4G modem task paused - modem_4g_init() skipped");
#endif

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Device init bundle complete");
    ESP_LOGI(TAG, "  BOARD=%s  NFC=%s  SD=%s  ETH=%s  4G=%s",
             devices->board_ready ? "ready" : "failed",
             devices->nfc_ready ? "ready" : "unavail",
             devices->sd_ready ? "mounted" : (sd_det == 0 ? "failed" : "absent"),
             devices->eth_ready ? "driver" : "unavail",
             devices->modem_started ? "started" : "paused");
    ESP_LOGI(TAG, "================================================");
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Traffic Light NFC System - main init");
    ESP_LOGI(TAG, "================================================");

    ESP_ERROR_CHECK(platform_init());
    ESP_ERROR_CHECK(devices_init(&s_devices));
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(time_sync_start());
    ESP_ERROR_CHECK(app_work_start(&s_devices));
    ESP_ERROR_CHECK(radar_input_start());
    ESP_ERROR_CHECK(web_config_start());
    ESP_ERROR_CHECK(peripheral_test_start());

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Main init complete; time sync + app work + test tasks are running");
    ESP_LOGI(TAG, "================================================");
}
