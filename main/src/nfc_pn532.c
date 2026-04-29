#include "nfc_pn532.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"
#include "pn532.h"
#include "pn532_driver_hsu.h"

static const char *TAG = "nfc_pn532";

static pn532_io_t s_io = {0};
static bool s_inited = false;

#define PN532_INIT_RETRIES 5
#define PN532_CANDIDATE_COUNT 1

typedef struct {
    gpio_num_t tx;
    gpio_num_t rx;
    int baud;
    const char *name;
} pn532_hsu_candidate_t;

static void clear_driver(void)
{
    pn532_release(&s_io);
    pn532_delete_driver(&s_io);
    memset(&s_io, 0, sizeof(s_io));
}

static esp_err_t try_hsu_candidate(const pn532_hsu_candidate_t *candidate)
{
    memset(&s_io, 0, sizeof(s_io));

    ESP_LOGI(TAG, "[1/4] HSU candidate '%s': UART%d TX=GPIO%d RX=GPIO%d baud=%d",
             candidate->name, NFC_UART_PORT, candidate->tx, candidate->rx, candidate->baud);

    esp_err_t err = pn532_new_driver_hsu(
        candidate->rx,
        candidate->tx,
        GPIO_NUM_NC,
        GPIO_NUM_NC,
        NFC_UART_PORT,
        candidate->baud,
        &s_io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[1/4] pn532_new_driver_hsu failed for '%s': %s (0x%x)",
                 candidate->name, esp_err_to_name(err), err);
        return err;
    }

    ESP_LOGI(TAG, "[2/4] pn532_init() for '%s' (%d retries)...",
             candidate->name, PN532_INIT_RETRIES);
    for (int attempt = 1; attempt <= PN532_INIT_RETRIES; ++attempt) {
        err = pn532_init(&s_io);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[2/4] PN532 hardware init OK with '%s' (attempt %d/%d)",
                     candidate->name, attempt, PN532_INIT_RETRIES);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "[2/4] '%s' attempt %d/%d failed: %s (0x%x)",
                 candidate->name, attempt, PN532_INIT_RETRIES, esp_err_to_name(err), err);
        pn532_release(&s_io);
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    ESP_LOGW(TAG, "[2/4] candidate '%s' failed after %d attempts",
             candidate->name, PN532_INIT_RETRIES);
    clear_driver();
    return err;
}

esp_err_t nfc_pn532_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "PN532 already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== PN532 NFC reader init start ===");

    /* PN532 requires up to 400 ms after power-on before it can respond.
     * Wait here in case we're called shortly after board power-up. */
    ESP_LOGI(TAG, "[0/4] Power-on settle delay 400 ms...");
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_LOGI(TAG, "[0/4] Settle done");

    const pn532_hsu_candidate_t candidates[PN532_CANDIDATE_COUNT] = {
        {PIN_NFC_TX, PIN_NFC_RX, NFC_UART_BAUD, "pin_map 115200"},
    };

    esp_err_t err = ESP_FAIL;
    const pn532_hsu_candidate_t *selected = NULL;
    for (size_t i = 0; i < PN532_CANDIDATE_COUNT; ++i) {
        err = try_hsu_candidate(&candidates[i]);
        if (err == ESP_OK) {
            selected = &candidates[i];
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[2/4] all PN532 HSU candidates failed; last error: %s (0x%x)",
                 esp_err_to_name(err), err);
        ESP_LOGE(TAG, "=== PN532 init FAILED ===");
        return err;
    }

    /* Step 3: Security Access Module config (normal mode, no virtual card) */
    ESP_LOGI(TAG, "[3/4] pn532_SAM_config() — setting normal mode (SAM bypassed)...");
    err = pn532_SAM_config(&s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[3/4] pn532_SAM_config failed: %s (0x%x)", esp_err_to_name(err), err);
        pn532_release(&s_io);
        pn532_delete_driver(&s_io);
        memset(&s_io, 0, sizeof(s_io));
        ESP_LOGE(TAG, "=== PN532 init FAILED ===");
        return err;
    }
    ESP_LOGI(TAG, "[3/4] SAM configured OK (normal mode)");

    /* Step 4: read firmware version (sanity check comms are working) */
    ESP_LOGI(TAG, "[4/4] pn532_get_firmware_version() — reading chip info...");
    uint32_t fw_version = 0;
    err = pn532_get_firmware_version(&s_io, &fw_version);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[4/4] pn532_get_firmware_version failed: %s (0x%x) — continuing anyway",
                 esp_err_to_name(err), err);
    } else {
        uint8_t ic      = (fw_version >> 24) & 0xFF;
        uint8_t ver     = (fw_version >> 16) & 0xFF;
        uint8_t rev     = (fw_version >> 8)  & 0xFF;
        uint8_t support = (fw_version)        & 0xFF;
        ESP_LOGI(TAG, "[4/4] Firmware: raw=0x%08" PRIx32
                 "  IC=0x%02x  Ver=%u.%u  Support=0x%02x",
                 fw_version, ic, ver, rev, support);
    }

    s_inited = true;
    ESP_LOGI(TAG, "=== PN532 ready via '%s' (UART%d TX=GPIO%d RX=GPIO%d baud=%d) ===",
             selected->name, NFC_UART_PORT, selected->tx, selected->rx, selected->baud);
    return ESP_OK;
}

void nfc_pn532_deinit(void)
{
    if (!s_inited) {
        return;
    }

    ESP_LOGI(TAG, "PN532 deinit...");
    pn532_release(&s_io);
    pn532_delete_driver(&s_io);
    memset(&s_io, 0, sizeof(s_io));
    s_inited = false;
    ESP_LOGI(TAG, "PN532 deinit done");
}
