#include "nfc_pn532.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"
#include "pn532.h"
#include "pn532_driver.h"
#include "pn532_driver_hsu.h"

static const char *TAG = "nfc_pn532";

static pn532_io_t s_io = {0};
static bool s_inited = false;
static TaskHandle_t s_scan_task = NULL;

#define PN532_INIT_RETRIES 5
#define PN532_CANDIDATE_COUNT 1
#define PN532_SCAN_TASK_STACK 4096
#define PN532_SCAN_TASK_PRIORITY 4
#define PN532_SCAN_TIMEOUT_MS 700
#define PN532_SCAN_INTERVAL_MS 300
#define PN532_NO_CARD_LOG_INTERVAL_MS 5000
#define PN532_CARD_READ_TIMEOUT_MS 500
#define PN532_MIFARE_CLASSIC_SECTOR1_BLOCK 4
#define PN532_NTAG_COMMAND_PAGE 4

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

static void log_uid(const uint8_t *uid, uint8_t uid_length)
{
    char uid_text[3 * 10] = {0};
    size_t pos = 0;

    for (uint8_t i = 0; i < uid_length && pos < sizeof(uid_text); ++i) {
        int n = snprintf(uid_text + pos, sizeof(uid_text) - pos,
                         "%s%02X", i == 0 ? "" : ":", uid[i]);
        if (n < 0 || n >= (int)(sizeof(uid_text) - pos)) {
            break;
        }
        pos += (size_t)n;
    }

    ESP_LOGI(TAG, "[SCAN] ISO14443A card detected: UID length=%u  UID=%s",
             uid_length, uid_text);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, uid, uid_length, ESP_LOG_INFO);
}

static bool same_uid(const uint8_t *a, uint8_t a_len, const uint8_t *b, uint8_t b_len)
{
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static esp_err_t pn532_data_exchange_tg1(const uint8_t *target_cmd,
                                         uint8_t target_cmd_len,
                                         uint8_t *response,
                                         uint8_t *response_len,
                                         int32_t timeout_ms)
{
    if (target_cmd == NULL || target_cmd_len == 0 || response == NULL || response_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cmd[32] = {
        PN532_COMMAND_INDATAEXCHANGE,
        1,
    };
    if (target_cmd_len > sizeof(cmd) - 2) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(cmd + 2, target_cmd, target_cmd_len);

    esp_err_t err = pn532_send_command_wait_ack(&s_io, cmd, target_cmd_len + 2, PN532_WRITE_TIMEOUT);
    if (err != ESP_OK) {
        return err;
    }

    err = pn532_wait_ready(&s_io, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t packet[64] = {0};
    err = pn532_read_data(&s_io, packet, sizeof(packet), PN532_READ_TIMEOUT);
    if (err != ESP_OK) {
        return err;
    }

    if (packet[0] != PN532_PREAMBLE || packet[1] != PN532_STARTCODE1 || packet[2] != PN532_STARTCODE2) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t length = packet[3];
    uint8_t length_checksum = packet[4];
    if (((length + length_checksum) & 0xFF) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if (length < 3 || packet[5] != PN532_PN532TOHOST || packet[6] != PN532_RESPONSE_INDATAEXCHANGE) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t status = packet[7];
    if ((status & 0x3F) != 0) {
        return ESP_FAIL;
    }

    uint8_t payload_len = length - 3;
    if (payload_len > *response_len) {
        payload_len = *response_len;
    }
    memcpy(response, packet + 8, payload_len);
    *response_len = payload_len;
    return ESP_OK;
}

static esp_err_t read_mifare_classic_sector1(uint8_t *uid,
                                             uint8_t uid_length,
                                             uint8_t *data,
                                             uint8_t data_len)
{
    if (uid == NULL || uid_length < 4 || data == NULL || data_len < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t default_key_a[6] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    const uint8_t *uid_auth = uid + uid_length - 4;
    uint8_t auth_cmd[12] = {
        MIFARE_CMD_AUTH_A,
        PN532_MIFARE_CLASSIC_SECTOR1_BLOCK,
    };
    memcpy(auth_cmd + 2, default_key_a, sizeof(default_key_a));
    memcpy(auth_cmd + 8, uid_auth, 4);

    uint8_t response[16] = {0};
    uint8_t response_len = sizeof(response);
    esp_err_t err = pn532_data_exchange_tg1(auth_cmd, sizeof(auth_cmd),
                                            response, &response_len,
                                            PN532_CARD_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t read_cmd[2] = {
        MIFARE_CMD_READ,
        PN532_MIFARE_CLASSIC_SECTOR1_BLOCK,
    };
    response_len = data_len;
    err = pn532_data_exchange_tg1(read_cmd, sizeof(read_cmd),
                                  data, &response_len,
                                  PN532_CARD_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return response_len >= 16 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t read_ntag_command_page(uint8_t *data, uint8_t data_len)
{
    if (data == NULL || data_len < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    return ntag2xx_read_page(&s_io, PN532_NTAG_COMMAND_PAGE, data, data_len);
}

static esp_err_t decode_command_value(const uint8_t *data, uint8_t *value)
{
    if (data == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (data[0] == '0' && data[1] >= '0' && data[1] <= '5') {
        *value = data[1] - '0';
        return ESP_OK;
    }

    if (data[0] == 0x00 && data[1] <= 0x05) {
        *value = data[1];
        return ESP_OK;
    }

    if (data[0] <= 0x05) {
        *value = data[0];
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static void pn532_scan_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "[SCAN] PN532 card scan task started");
    ESP_LOGI(TAG, "[SCAN] Present an ISO14443A card/tag near the PN532 antenna");

    esp_err_t err = pn532_set_passive_activation_retries(&s_io, 0x01);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[SCAN] set passive activation retries failed: %s (0x%x)",
                 esp_err_to_name(err), err);
    }

    uint8_t last_uid[10] = {0};
    uint8_t last_uid_len = 0;
    uint32_t last_no_card_log_ms = 0;
    bool card_present = false;

    while (s_inited) {
        uint8_t uid[10] = {0};
        uint8_t uid_length = 0;
        err = pn532_read_passive_target_id(&s_io,
                                           PN532_BRTY_ISO14443A_106KBPS,
                                           uid,
                                           &uid_length,
                                           PN532_SCAN_TIMEOUT_MS);

        if (err == ESP_OK && uid_length > 0 && uid_length <= sizeof(uid)) {
            if (!card_present || !same_uid(uid, uid_length, last_uid, last_uid_len)) {
                log_uid(uid, uid_length);
                memcpy(last_uid, uid, uid_length);
                last_uid_len = uid_length;
            }
            card_present = true;
        } else {
            uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (card_present) {
                ESP_LOGI(TAG, "[SCAN] Card removed");
                card_present = false;
                last_uid_len = 0;
                memset(last_uid, 0, sizeof(last_uid));
            } else if (now_ms - last_no_card_log_ms >= PN532_NO_CARD_LOG_INTERVAL_MS) {
                ESP_LOGI(TAG, "[SCAN] No ISO14443A card detected yet");
                last_no_card_log_ms = now_ms;
            }

            if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_FAIL) {
                ESP_LOGW(TAG, "[SCAN] pn532_read_passive_target_id failed: %s (0x%x)",
                         esp_err_to_name(err), err);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PN532_SCAN_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "[SCAN] PN532 card scan task stopped");
    s_scan_task = NULL;
    vTaskDelete(NULL);
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

esp_err_t nfc_pn532_start_card_scan(void)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "PN532 scan requested before init");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_scan_task != NULL) {
        ESP_LOGI(TAG, "PN532 card scan task already running");
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(pn532_scan_task,
                                            "pn532_scan",
                                            PN532_SCAN_TASK_STACK,
                                            NULL,
                                            PN532_SCAN_TASK_PRIORITY,
                                            &s_scan_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_scan_task = NULL;
        ESP_LOGE(TAG, "Failed to create PN532 card scan task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "PN532 card scan task created");
    return ESP_OK;
}

esp_err_t nfc_pn532_read_card_command(nfc_card_command_t *card)
{
    if (card == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(card, 0, sizeof(*card));

    esp_err_t err = pn532_read_passive_target_id(&s_io,
                                                 PN532_BRTY_ISO14443A_106KBPS,
                                                 card->uid,
                                                 &card->uid_length,
                                                 PN532_CARD_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    card->data_length = sizeof(card->data);
    err = read_mifare_classic_sector1(card->uid, card->uid_length,
                                      card->data, card->data_length);
    if (err == ESP_OK) {
        card->source = "mifare_classic_sector1_block4";
    } else {
        ESP_LOGW(TAG, "[CARD] Mifare Classic sector1 read failed: %s (0x%x); trying NTAG page %d",
                 esp_err_to_name(err), err, PN532_NTAG_COMMAND_PAGE);
        memset(card->data, 0, sizeof(card->data));
        err = read_ntag_command_page(card->data, sizeof(card->data));
        if (err != ESP_OK) {
            return err;
        }
        card->source = "ntag_page4";
    }

    card->data_length = sizeof(card->data);
    err = decode_command_value(card->data, &card->command_value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[CARD] Unsupported command bytes: %02X %02X",
                 card->data[0], card->data[1]);
        return err;
    }

    log_uid(card->uid, card->uid_length);
    ESP_LOGI(TAG, "[CARD] source=%s data[0..1]=%02X %02X command=%u",
             card->source, card->data[0], card->data[1], card->command_value);
    return ESP_OK;
}

void nfc_pn532_deinit(void)
{
    if (!s_inited) {
        return;
    }

    ESP_LOGI(TAG, "PN532 deinit...");
    s_inited = false;
    if (s_scan_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(PN532_SCAN_TIMEOUT_MS + PN532_SCAN_INTERVAL_MS + 100));
    }
    pn532_release(&s_io);
    pn532_delete_driver(&s_io);
    memset(&s_io, 0, sizeof(s_io));
    ESP_LOGI(TAG, "PN532 deinit done");
}
