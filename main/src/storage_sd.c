#include "storage_sd.h"

#include <stdio.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "pin_map.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage_sd";
static const char *MOUNT_POINT = "/sdcard";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

static esp_err_t mount_card(sdmmc_slot_config_t *slot_cfg, const char *attempt)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s): %s", attempt, esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s (%s)", MOUNT_POINT, attempt);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t storage_sd_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    slot_cfg.clk = PIN_SD_CLK;
    slot_cfg.cmd = PIN_SD_CMD;
    slot_cfg.d0 = PIN_SD_D0;
    slot_cfg.d1 = GPIO_NUM_NC;
    slot_cfg.d2 = GPIO_NUM_NC;
    slot_cfg.d3 = GPIO_NUM_NC;
    slot_cfg.d4 = GPIO_NUM_NC;
    slot_cfg.d5 = GPIO_NUM_NC;
    slot_cfg.d6 = GPIO_NUM_NC;
    slot_cfg.d7 = GPIO_NUM_NC;
    slot_cfg.gpio_wp = GPIO_NUM_NC;
    slot_cfg.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* First try with card-detect from schematic (SDMMC_DET=GPIO1). */
    slot_cfg.gpio_cd = PIN_SD_DET;
    esp_err_t err = mount_card(&slot_cfg, "gpio_cd");
    if (err == ESP_OK) {
        return ESP_OK;
    }

    /* Fallback for boards where CD line polarity or wiring differs. */
    slot_cfg.gpio_cd = SDMMC_SLOT_NO_CD;
    err = mount_card(&slot_cfg, "no-gpio_cd fallback");
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t storage_sd_probe_rw(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[64] = {0};
    snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, ".probe.txt");

    char payload[64] = {0};
    snprintf(payload, sizeof(payload), "probe_us=%lld\n", (long long)esp_timer_get_time());

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "open for write failed: %s", path);
        return ESP_FAIL;
    }

    if (fputs(payload, f) < 0) {
        fclose(f);
        ESP_LOGE(TAG, "write probe failed: %s", path);
        return ESP_FAIL;
    }
    fclose(f);

    char readback[96] = {0};
    f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "open for read failed: %s", path);
        return ESP_FAIL;
    }

    if (fgets(readback, sizeof(readback), f) == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "read probe failed: %s", path);
        return ESP_FAIL;
    }
    fclose(f);

    if (strcmp(payload, readback) != 0) {
        ESP_LOGE(TAG, "probe mismatch: wrote='%s' read='%s'", payload, readback);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SD probe OK: %s", readback);
    return ESP_OK;
}

esp_err_t storage_sd_deinit(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD unmount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "SD unmounted from %s", MOUNT_POINT);
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
