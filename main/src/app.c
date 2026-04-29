#include "app.h"

#include "board_hal.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem_4g.h"
#include "net_eth.h"
#include "storage_sd.h"

static const char *TAG = "app";

void app_start(void)
{
    ESP_LOGI(TAG, "Booting hardware init from schematic...");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(board_hal_init());
    board_hal_log_map();
    ESP_ERROR_CHECK(modem_4g_init());

    err = storage_sd_init();
    if (err == ESP_OK) {
        err = storage_sd_probe_rw();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD mounted but probe failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "SD not available at boot: %s", esp_err_to_name(err));
    }

    err = net_eth_init();
    if (err == ESP_OK) {
        err = net_eth_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ETH init OK but start failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "ETH not available at boot: %s", esp_err_to_name(err));
    }

    while (1) {
        board_inputs_t in = {0};
        ESP_ERROR_CHECK(board_hal_read_inputs(&in));

        ESP_LOGI(TAG, "K1=%d K2=%d SD_DET=%d ETH_INT=%d SD_MOUNTED=%d ETH_STARTED=%d",
                 in.key1, in.key2, in.sd_det, in.eth_int, storage_sd_is_mounted(), net_eth_is_started());

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
