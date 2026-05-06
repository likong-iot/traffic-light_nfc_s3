#include "app.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "board_hal.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem_4g.h"
#include "net_eth.h"
#include "nfc_pn532.h"
#include "nvs_flash.h"
#include "peripheral_test.h"
#include "pin_map.h"
#include "storage_sd.h"

static const char *TAG = "app";

/*
 * Bring-up switch: keep the AIR780E modem code in the project, but do not
 * start its background state machine while PN532/SD/IO tests are being run.
 */
#ifndef APP_ENABLE_4G_MODEM
#define APP_ENABLE_4G_MODEM 1
#endif

#define APP_WORK_TASK_STACK      8192
#define APP_WORK_TASK_PRIORITY   3
#define APP_WORK_LOG_INTERVAL_MS 10000
#define APP_NFC_POLL_INTERVAL_MS 300
#define APP_IO_PULSE_HIGH_MS     50
#define APP_IO_PULSE_LOW_GAP_MS  50

typedef struct {
    bool board_ready;
    bool nfc_ready;
    bool sd_ready;
    bool eth_ready;
    bool modem_started;
    int sd_det_at_init;
} app_devices_t;

static app_devices_t s_devices = {0};
static TaskHandle_t s_work_task = NULL;

static void bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    size_t pos = 0;
    if (out_len == 0) {
        return;
    }
    out[0] = '\0';

    for (size_t i = 0; i < len && pos < out_len; ++i) {
        int n = snprintf(out + pos, out_len - pos, "%s%02X", i == 0 ? "" : ":", data[i]);
        if (n < 0 || n >= (int)(out_len - pos)) {
            break;
        }
        pos += (size_t)n;
    }
}

static esp_err_t app_append_sd_log(const char *line)
{
    if (line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_sd_is_mounted()) {
        ESP_LOGW(TAG, "[work] SD not mounted, log skipped: %s", line);
        return ESP_ERR_INVALID_STATE;
    }

    char path[64];
    int path_len = snprintf(path, sizeof(path), "%s/NFCLOG.TXT", storage_sd_mount_point());
    if (path_len < 0 || path_len >= (int)sizeof(path)) {
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "[work] open log '%s' failed: errno=%d (%s)",
                 path, errno, strerror(errno));
        return ESP_FAIL;
    }

    if (fprintf(f, "%s\r\n", line) < 0) {
        ESP_LOGE(TAG, "[work] write log failed: errno=%d (%s)", errno, strerror(errno));
        fclose(f);
        return ESP_FAIL;
    }
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(f);
    return ESP_OK;
}

static void app_log_nfc_event(const char *event,
                              const nfc_card_command_t *card,
                              esp_err_t result,
                              int pulse_count)
{
    char uid_hex[3 * 10] = {0};
    char data_hex[3 * 16] = {0};
    char line[256];

    if (card != NULL) {
        bytes_to_hex(card->uid, card->uid_length, uid_hex, sizeof(uid_hex));
        bytes_to_hex(card->data, card->data_length, data_hex, sizeof(data_hex));
    } else {
        strcpy(uid_hex, "-");
        strcpy(data_hex, "-");
    }

    snprintf(line, sizeof(line),
             "tick_ms=%lu,event=%s,uid=%s,source=%s,data=%s,cmd=%d,pulses=%d,result=%s",
             (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
             event ? event : "-",
             uid_hex,
             card && card->source ? card->source : "-",
             data_hex,
             card ? (int)card->command_value : -1,
             pulse_count,
             esp_err_to_name(result));

    ESP_LOGI(TAG, "[work] log: %s", line);
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_append_sd_log(line));
}

static bool same_card_uid(const nfc_card_command_t *card, const uint8_t *last_uid, uint8_t last_uid_len)
{
    return card != NULL &&
           card->uid_length == last_uid_len &&
           memcmp(card->uid, last_uid, card->uid_length) == 0;
}

static esp_err_t app_platform_init(void)
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

static esp_err_t app_devices_init(app_devices_t *devices)
{
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
        ESP_LOGW(TAG, "[devices 2/5] PN532 init failed (%s) — NFC unavailable",
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
        ESP_LOGW(TAG, "[devices 3/5] SD init failed (%s) — storage unavailable",
                 esp_err_to_name(err));
    } else {
        devices->sd_ready = true;
        ESP_LOGI(TAG, "[devices 3/5] SD ready at '%s'", storage_sd_mount_point());
    }

    ESP_LOGI(TAG, "[devices 4/5] W5500 Ethernet init (SPI2, direct driver — no ethernet_init component)");
    err = net_eth_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[devices 4/5] W5500 init failed (%s) — ethernet unavailable",
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
        ESP_LOGI(TAG, "[devices 5/5] 4G modem task started — connecting in background");
    }
#else
    ESP_LOGW(TAG, "[devices 5/5] AIR780ER 4G modem task paused — modem_4g_init() skipped");
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

static void app_work_task(void *arg)
{
    app_devices_t *devices = (app_devices_t *)arg;

    ESP_LOGI(TAG, "=== app work task started ===");
    ESP_LOGI(TAG, "Work task owns NFC command loop, IO_OUT1 pulses, SD logging, and periodic status");
    ESP_LOGI(TAG, "[work] NFC command rule: 00=>1 pulse, 01=>2 pulses, 02=>3 pulses");

    uint8_t last_uid[10] = {0};
    uint8_t last_uid_len = 0;
    bool card_latched = false;
    uint32_t last_status_log_ms = 0;

    while (1) {
        if (devices->nfc_ready) {
            nfc_card_command_t card = {0};
            esp_err_t err = nfc_pn532_read_card_command(&card);
            if (err == ESP_OK) {
                if (!card_latched || !same_card_uid(&card, last_uid, last_uid_len)) {
                    int pulse_count = (int)card.command_value + 1;
                    ESP_LOGI(TAG, "[work] NFC command %02u -> IO_OUT1 %d pulse(s)",
                             card.command_value, pulse_count);
                    app_log_nfc_event("card_read", &card, ESP_OK, pulse_count);

                    esp_err_t pulse_err = board_hal_pulse_io_out1(pulse_count,
                                                                  APP_IO_PULSE_HIGH_MS,
                                                                  APP_IO_PULSE_LOW_GAP_MS);
                    app_log_nfc_event("io_out1_pulse", &card, pulse_err, pulse_count);

                    memcpy(last_uid, card.uid, card.uid_length);
                    last_uid_len = card.uid_length;
                    card_latched = true;
                }
            } else if (err == ESP_ERR_NOT_FOUND) {
                if (card_latched) {
                    ESP_LOGI(TAG, "[work] NFC card removed; next card read will trigger again");
                }
                card_latched = false;
                last_uid_len = 0;
                memset(last_uid, 0, sizeof(last_uid));
            } else {
                ESP_LOGW(TAG, "[work] NFC command read failed: %s", esp_err_to_name(err));
                app_log_nfc_event("card_read_failed", &card, err, 0);
            }
        }

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_status_log_ms >= APP_WORK_LOG_INTERVAL_MS) {
            board_inputs_t inputs = {0};
            if (devices->board_ready && board_hal_read_inputs(&inputs) == ESP_OK) {
                ESP_LOGI(TAG, "[work] inputs: KEY1=%d KEY2=%d SD_DET=%d ETH_INT=%d",
                         inputs.key1, inputs.key2, inputs.sd_det, inputs.eth_int);
            }

            ESP_LOGI(TAG, "[work] status: NFC=%s SD=%s ETH=%s 4G=%s",
                     devices->nfc_ready ? "ready" : "unavail",
                     storage_sd_is_mounted() ? "mounted" : "unmounted",
                     net_eth_is_connected() ? "linked" : (devices->eth_ready ? "waiting" : "unavail"),
                     modem_4g_is_connected() ? "connected" : (devices->modem_started ? "connecting" : "paused"));
            last_status_log_ms = now_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_NFC_POLL_INTERVAL_MS));
    }
}

static esp_err_t app_work_start(app_devices_t *devices)
{
    if (s_work_task != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting app work task: stack=%d prio=%d",
             APP_WORK_TASK_STACK, APP_WORK_TASK_PRIORITY);
    BaseType_t ok = xTaskCreatePinnedToCore(app_work_task,
                                            "app_work",
                                            APP_WORK_TASK_STACK,
                                            devices,
                                            APP_WORK_TASK_PRIORITY,
                                            &s_work_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_work_task = NULL;
        ESP_LOGE(TAG, "Failed to create app work task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_start(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Traffic Light NFC System — main init");
    ESP_LOGI(TAG, "================================================");

    ESP_ERROR_CHECK(app_platform_init());
    ESP_ERROR_CHECK(app_devices_init(&s_devices));
    ESP_ERROR_CHECK(app_work_start(&s_devices));
    ESP_ERROR_CHECK(peripheral_test_start());

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Main init complete; app work + test tasks are running");
    ESP_LOGI(TAG, "================================================");
}
