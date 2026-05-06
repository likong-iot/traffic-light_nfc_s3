#include "storage_sd.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_map.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage_sd";
static const char *MOUNT_POINT = "/sdcard";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

static const esp_vfs_fat_mount_config_t s_mount_cfg = {
    .format_if_mount_failed = false,
    .max_files = 8,
    .allocation_unit_size = 16 * 1024,
    .disk_status_check_enable = true,
    .use_one_fat = false,
};

typedef struct {
    int det;
    int cmd;
    int d0;
    int clk;
} sd_pin_levels_t;

static sd_pin_levels_t read_sd_pin_levels(void)
{
    sd_pin_levels_t levels = {
        .det = gpio_get_level(PIN_SD_DET),
        .cmd = gpio_get_level(PIN_SD_CMD),
        .d0 = gpio_get_level(PIN_SD_D0),
        .clk = gpio_get_level(PIN_SD_CLK),
    };
    return levels;
}

static void log_sd_pin_levels(const char *stage)
{
    sd_pin_levels_t levels = read_sd_pin_levels();
    ESP_LOGI(TAG, "%s: DET(GPIO%d)=%d CMD(GPIO%d)=%d D0(GPIO%d)=%d CLK(GPIO%d)=%d",
             stage,
             PIN_SD_DET, levels.det,
             PIN_SD_CMD, levels.cmd,
             PIN_SD_D0, levels.d0,
             PIN_SD_CLK, levels.clk);
}

static esp_err_t config_input_pullup(gpio_num_t gpio)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
}

static void config_bus_inputs(gpio_pullup_t pullup)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(PIN_SD_CMD) | BIT64(PIN_SD_D0) | BIT64(PIN_SD_CLK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io_conf));
}

static void prepare_sd_bus_pins(void)
{
    gpio_num_t pins[] = {
        PIN_SD_CMD,
        PIN_SD_D0,
        PIN_SD_CLK,
    };

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }

    /*
     * GPIO1 is not DAT3/CS on this board. It is the active-low card detect
     * signal and also enables the socket power switch in hardware.
     */
    ESP_ERROR_CHECK_WITHOUT_ABORT(config_input_pullup(PIN_SD_DET));

    config_bus_inputs(GPIO_PULLUP_ENABLE);
}

static esp_err_t mount_sdmmc_1bit(void)
{
    ESP_LOGI(TAG, "SDMMC 1-bit attempt: CLK=GPIO%d CMD=GPIO%d D0=GPIO%d CD/DET=GPIO%d",
             PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_DET);
    prepare_sd_bus_pins();

    sd_pin_levels_t idle = read_sd_pin_levels();
    if (idle.det != 0) {
        ESP_LOGW(TAG, "SD_DET is high: card is not detected, skip mount");
        return ESP_ERR_NOT_FOUND;
    }
    if (idle.d0 == 0) {
        ESP_LOGE(TAG, "DAT0/D0 is held low before init. SDMMC cannot start while DAT0 is busy/low.");
        ESP_LOGE(TAG, "Check hardware: SD_VCC, R15 pull-up, socket DAT0 contact, card state, and GPIO2 routing.");
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.max_freq_khz = SDMMC_FREQ_PROBING;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    slot_cfg.clk = PIN_SD_CLK;
    slot_cfg.cmd = PIN_SD_CMD;
    slot_cfg.d0 = PIN_SD_D0;
    slot_cfg.d1 = GPIO_NUM_NC;
    slot_cfg.d2 = GPIO_NUM_NC;
    slot_cfg.d3 = GPIO_NUM_NC;
    slot_cfg.gpio_cd = PIN_SD_DET;
    slot_cfg.gpio_wp = SDMMC_SLOT_NO_WP;
    slot_cfg.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_cfg, &s_mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SDMMC 1-bit mount failed: %s (0x%x)", esp_err_to_name(err), err);
        log_sd_pin_levels("SD pin levels after failed mount");
        s_card = NULL;
        return err;
    }

    ESP_LOGI(TAG, "SDMMC 1-bit mount OK");
    return ESP_OK;
}

static esp_err_t storage_sd_rw_self_test(void)
{
    char path[64];
    /*
     * sdkconfig currently has CONFIG_FATFS_LFN_NONE=y, so keep the test file
     * in 8.3 format. This should show up on a PC as SDTEST.TXT.
     */
    int path_len = snprintf(path, sizeof(path), "%s/SDTEST.TXT", MOUNT_POINT);
    if (path_len < 0 || path_len >= (int)sizeof(path)) {
        ESP_LOGE(TAG, "SD R/W self-test path format failed");
        return ESP_FAIL;
    }

    char expected[160];
    int written = snprintf(expected, sizeof(expected),
                           "traffic-light_nfc_s3 SD read/write self-test\r\n"
                           "mount=%s\r\n"
                           "tick_ms=%lu\r\n",
                           MOUNT_POINT,
                           (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    if (written < 0 || written >= (int)sizeof(expected)) {
        ESP_LOGE(TAG, "SD R/W self-test payload format failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[R/W] Creating test file: %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "[R/W] fopen('%s','w') failed: errno=%d (%s)",
                 path, errno, strerror(errno));
        return ESP_FAIL;
    }

    size_t expected_len = strlen(expected);
    size_t write_len = fwrite(expected, 1, expected_len, f);
    if (write_len != expected_len) {
        ESP_LOGE(TAG, "[R/W] fwrite failed: wrote=%u expected=%u errno=%d (%s)",
                 (unsigned)write_len, (unsigned)expected_len, errno, strerror(errno));
        fclose(f);
        return ESP_FAIL;
    }

    if (fflush(f) != 0) {
        ESP_LOGE(TAG, "[R/W] fflush failed: errno=%d (%s)", errno, strerror(errno));
        fclose(f);
        return ESP_FAIL;
    }

    int fd = fileno(f);
    if (fd >= 0 && fsync(fd) != 0) {
        ESP_LOGE(TAG, "[R/W] fsync failed: errno=%d (%s)", errno, strerror(errno));
        fclose(f);
        return ESP_FAIL;
    }

    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "[R/W] fclose after write failed: errno=%d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[R/W] Write OK: %u bytes", (unsigned)expected_len);

    ESP_LOGI(TAG, "[R/W] Re-opening test file for readback");
    f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "[R/W] fopen('%s','r') failed: errno=%d (%s)",
                 path, errno, strerror(errno));
        return ESP_FAIL;
    }

    char actual[sizeof(expected)] = {0};
    size_t read_len = fread(actual, 1, sizeof(actual) - 1, f);
    if (ferror(f)) {
        ESP_LOGE(TAG, "[R/W] fread failed: errno=%d (%s)", errno, strerror(errno));
        fclose(f);
        return ESP_FAIL;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "[R/W] fclose after read failed: errno=%d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }
    actual[read_len] = '\0';

    if (read_len != expected_len || strcmp(actual, expected) != 0) {
        ESP_LOGE(TAG, "[R/W] Readback mismatch: read=%u expected=%u",
                 (unsigned)read_len, (unsigned)expected_len);
        ESP_LOGE(TAG, "[R/W] Expected: %s", expected);
        ESP_LOGE(TAG, "[R/W] Actual: %s", actual);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "[R/W] Readback OK: %u bytes matched", (unsigned)read_len);
    ESP_LOGI(TAG, "[R/W] SD card read/write self-test PASSED");
    return ESP_OK;
}

static void storage_sd_log_root_dir(void)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "Could not open SD root '%s' for listing: errno=%d (%s)",
                 MOUNT_POINT, errno, strerror(errno));
        return;
    }

    ESP_LOGI(TAG, "SD root directory listing:");
    struct dirent *entry = NULL;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "  %s", entry->d_name);
        ++count;
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  <empty>");
    }
}

esp_err_t storage_sd_init(void)
{
    if (s_mounted) {
        ESP_LOGI(TAG, "SD already mounted at %s", MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== SD card init start ===");

    ESP_LOGI(TAG, "Board SD wiring: native SDMMC 1-bit only; DET is not DAT3/CS");
    ESP_LOGI(TAG, "SD_DET probe level before mount: GPIO%d=%d", PIN_SD_DET, gpio_get_level(PIN_SD_DET));
    ESP_LOGI(TAG, "VFS FAT mount config: path='%s'  max_files=%d  alloc_unit=%d B"
             "  format_if_fail=no  disk_status_check=yes",
             MOUNT_POINT, s_mount_cfg.max_files, s_mount_cfg.allocation_unit_size);

    esp_err_t err = mount_sdmmc_1bit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (0x%x)", esp_err_to_name(err), err);
        ESP_LOGE(TAG, "=== SD card init FAILED ===");
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "[3/3] SD mounted OK at '%s'", MOUNT_POINT);
    ESP_LOGI(TAG, "       Card name:     %s", s_card->cid.name);
    ESP_LOGI(TAG, "       Negotiated speed: %u kHz", s_card->max_freq_khz);
    ESP_LOGI(TAG, "       Capacity:      %lluMB",
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024ULL * 1024ULL));
    ESP_LOGI(TAG, "       Sector size:   %u B", s_card->csd.sector_size);
    ESP_LOGI(TAG, "       CSD ver:       %d", s_card->csd.csd_ver);
    sdmmc_card_print_info(stdout, s_card);

    ESP_LOGI(TAG, "=== SD card init done (self-test deferred) ===");
    return ESP_OK;
}

esp_err_t storage_sd_self_test(void)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD self-test skipped: card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "=== SD card self-test start ===");
    esp_err_t err = storage_sd_rw_self_test();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD read/write self-test failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }
    storage_sd_log_root_dir();
    ESP_LOGI(TAG, "=== SD card self-test done ===");
    return ESP_OK;
}

esp_err_t storage_sd_deinit(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unmounting SD from '%s'...", MOUNT_POINT);
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD unmount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "SD unmounted OK");
    return ESP_OK;
}

bool storage_sd_is_mounted(void)
{
    return s_mounted;
}

const char *storage_sd_mount_point(void)
{
    return MOUNT_POINT;
}
