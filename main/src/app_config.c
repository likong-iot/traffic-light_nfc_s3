#include "app_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs.h"
#include "storage_sd.h"

static const char *TAG = "app_config";

#define APP_CONFIG_NVS_NAMESPACE   "app_cfg"
#define APP_CONFIG_NVS_KEY_JSON    "json"
#define APP_CONFIG_SD_FILENAME     "/CONFIG.JSN"
#define APP_CONFIG_MAX_JSON_SIZE   8192

static app_config_t s_config;
static bool s_inited = false;

static void app_config_apply_current_policy(app_config_t *cfg)
{
    if (cfg == NULL || cfg->version >= APP_CONFIG_VERSION) {
        return;
    }

    for (size_t i = 0; i < cfg->nfc_rule_count; ++i) {
        if (strcmp(cfg->nfc_rules[i].data, "02") == 0 ||
            strcmp(cfg->nfc_rules[i].data, "03") == 0 ||
            strcmp(cfg->nfc_rules[i].data, "04") == 0 ||
            strcmp(cfg->nfc_rules[i].data, "05") == 0) {
            cfg->nfc_rules[i].open_relay4 = true;
        }
    }
    cfg->version = APP_CONFIG_VERSION;
}

static void app_config_set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = APP_CONFIG_VERSION;
    cfg->schedule.relay1_start_min = 18 * 60;
    cfg->schedule.relay1_end_min = 7 * 60;
    cfg->schedule.relay2_start_min = 18 * 60;
    cfg->schedule.relay2_end_min = 7 * 60;
    cfg->radar.enabled = true;
    cfg->radar.active_level = 1;
    cfg->radar.trigger_delay_ms = 5000;
    cfg->radar.cycle_window_ms = 20000;
    cfg->radar.opto12_pulses = 1;

    cfg->nfc_rule_count = 6;
    for (size_t i = 0; i < cfg->nfc_rule_count; ++i) {
        snprintf(cfg->nfc_rules[i].data, sizeof(cfg->nfc_rules[i].data), "%02X", (unsigned)i);
        snprintf(cfg->nfc_rules[i].name, sizeof(cfg->nfc_rules[i].name), "类别%u卡", (unsigned)(i + 1));
        cfg->nfc_rules[i].opto12_pulses = (int)i + 1;
        cfg->nfc_rules[i].open_relay4 = (i >= 2);
    }
    cfg->source = APP_CONFIG_SOURCE_DEFAULT;
}

static int parse_minutes_text(const char *text)
{
    if (text == NULL || strlen(text) != 5 || text[2] != ':') {
        return -1;
    }
    if (!isdigit((unsigned char)text[0]) || !isdigit((unsigned char)text[1]) ||
        !isdigit((unsigned char)text[3]) || !isdigit((unsigned char)text[4])) {
        return -1;
    }
    int hour = (text[0] - '0') * 10 + (text[1] - '0');
    int minute = (text[3] - '0') * 10 + (text[4] - '0');
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return -1;
    }
    return hour * 60 + minute;
}

static void format_minutes(int minutes, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    minutes %= 24 * 60;
    if (minutes < 0) {
        minutes += 24 * 60;
    }
    snprintf(out, out_len, "%02d:%02d", minutes / 60, minutes % 60);
}

static bool parse_bool(cJSON *node, bool *value)
{
    if (cJSON_IsBool(node)) {
        *value = cJSON_IsTrue(node);
        return true;
    }
    if (cJSON_IsNumber(node)) {
        *value = node->valueint != 0;
        return true;
    }
    return false;
}

static bool parse_int(cJSON *node, int *value)
{
    if (!cJSON_IsNumber(node)) {
        return false;
    }
    *value = node->valueint;
    return true;
}

static bool parse_time_text(cJSON *node, int *minutes)
{
    if (!cJSON_IsString(node) || node->valuestring == NULL) {
        return false;
    }
    int parsed = parse_minutes_text(node->valuestring);
    if (parsed < 0) {
        return false;
    }
    *minutes = parsed;
    return true;
}

static bool is_hex_text_pair(const char *text)
{
    return text != NULL && strlen(text) == 2 &&
           isxdigit((unsigned char)text[0]) &&
           isxdigit((unsigned char)text[1]);
}

static bool parse_json_rule(cJSON *node, app_nfc_rule_t *rule)
{
    if (!cJSON_IsObject(node) || rule == NULL) {
        return false;
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(node, "data");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(node, "name");
    cJSON *pulses = cJSON_GetObjectItemCaseSensitive(node, "opto12_pulses");
    cJSON *open_relay4 = cJSON_GetObjectItemCaseSensitive(node, "open_relay4");

    if (!cJSON_IsString(data) || !is_hex_text_pair(data->valuestring)) {
        return false;
    }
    rule->data[0] = (char)toupper((unsigned char)data->valuestring[0]);
    rule->data[1] = (char)toupper((unsigned char)data->valuestring[1]);
    rule->data[2] = '\0';

    if (cJSON_IsString(name) && name->valuestring != NULL) {
        strlcpy(rule->name, name->valuestring, sizeof(rule->name));
    } else {
        rule->name[0] = '\0';
    }

    if (!parse_int(pulses, &rule->opto12_pulses) || rule->opto12_pulses <= 0) {
        return false;
    }
    if (!parse_bool(open_relay4, &rule->open_relay4)) {
        rule->open_relay4 = false;
    }
    return true;
}

static bool load_from_json_text(app_config_t *cfg, const char *json)
{
    if (cfg == NULL || json == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse failed");
        return false;
    }

    bool ok = true;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsNumber(version) && version->valueint > 0) {
        cfg->version = version->valueint;
    }

    cJSON *schedule = cJSON_GetObjectItemCaseSensitive(root, "schedule");
    if (cJSON_IsObject(schedule)) {
        cJSON *relay1 = cJSON_GetObjectItemCaseSensitive(schedule, "relay1");
        cJSON *relay2 = cJSON_GetObjectItemCaseSensitive(schedule, "relay2");
        if (cJSON_IsObject(relay1)) {
            cJSON *start = cJSON_GetObjectItemCaseSensitive(relay1, "start");
            cJSON *end = cJSON_GetObjectItemCaseSensitive(relay1, "end");
            ok &= parse_time_text(start, &cfg->schedule.relay1_start_min);
            ok &= parse_time_text(end, &cfg->schedule.relay1_end_min);
        }
        if (cJSON_IsObject(relay2)) {
            cJSON *start = cJSON_GetObjectItemCaseSensitive(relay2, "start");
            cJSON *end = cJSON_GetObjectItemCaseSensitive(relay2, "end");
            ok &= parse_time_text(start, &cfg->schedule.relay2_start_min);
            ok &= parse_time_text(end, &cfg->schedule.relay2_end_min);
        }
    }

    cJSON *radar = cJSON_GetObjectItemCaseSensitive(root, "radar");
    if (cJSON_IsObject(radar)) {
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(radar, "enabled");
        cJSON *active_level = cJSON_GetObjectItemCaseSensitive(radar, "active_level");
        cJSON *trigger_delay_ms = cJSON_GetObjectItemCaseSensitive(radar, "trigger_delay_ms");
        cJSON *cycle_window_ms = cJSON_GetObjectItemCaseSensitive(radar, "cycle_window_ms");
        cJSON *opto12_pulses = cJSON_GetObjectItemCaseSensitive(radar, "opto12_pulses");
        if (enabled != NULL) ok &= parse_bool(enabled, &cfg->radar.enabled);
        if (active_level != NULL) ok &= parse_int(active_level, &cfg->radar.active_level);
        if (trigger_delay_ms != NULL) ok &= parse_int(trigger_delay_ms, &cfg->radar.trigger_delay_ms);
        if (cycle_window_ms != NULL) ok &= parse_int(cycle_window_ms, &cfg->radar.cycle_window_ms);
        if (opto12_pulses != NULL) ok &= parse_int(opto12_pulses, &cfg->radar.opto12_pulses);
    }

    cJSON *nfc = cJSON_GetObjectItemCaseSensitive(root, "nfc");
    if (cJSON_IsObject(nfc)) {
        cJSON *rules = cJSON_GetObjectItemCaseSensitive(nfc, "rules");
        if (cJSON_IsArray(rules)) {
            size_t count = 0;
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, rules) {
                if (count >= APP_CONFIG_MAX_NFC_RULES) {
                    break;
                }
                app_nfc_rule_t rule = {0};
                if (!parse_json_rule(item, &rule)) {
                    ok = false;
                    continue;
                }
                cfg->nfc_rules[count++] = rule;
            }
            if (count > 0) {
                cfg->nfc_rule_count = count;
            }
        }
    }

    cJSON_Delete(root);
    if (ok) {
        app_config_apply_current_policy(cfg);
    }
    return ok;
}

static bool load_json_file(const char *path, char *buffer, size_t buffer_len)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    size_t read_len = fread(buffer, 1, buffer_len - 1, file);
    fclose(file);
    buffer[read_len] = '\0';
    return read_len > 0;
}

static bool save_json_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, file);
    fflush(file);
    fclose(file);
    return written == len;
}

static char *create_json_text(const app_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", cfg->version);

    cJSON *schedule = cJSON_AddObjectToObject(root, "schedule");
    cJSON *relay1 = cJSON_AddObjectToObject(schedule, "relay1");
    cJSON *relay2 = cJSON_AddObjectToObject(schedule, "relay2");
    char time_text[6] = {0};
    format_minutes(cfg->schedule.relay1_start_min, time_text, sizeof(time_text));
    cJSON_AddStringToObject(relay1, "start", time_text);
    format_minutes(cfg->schedule.relay1_end_min, time_text, sizeof(time_text));
    cJSON_AddStringToObject(relay1, "end", time_text);
    format_minutes(cfg->schedule.relay2_start_min, time_text, sizeof(time_text));
    cJSON_AddStringToObject(relay2, "start", time_text);
    format_minutes(cfg->schedule.relay2_end_min, time_text, sizeof(time_text));
    cJSON_AddStringToObject(relay2, "end", time_text);

    cJSON *nfc = cJSON_AddObjectToObject(root, "nfc");
    cJSON *rules = cJSON_AddArrayToObject(nfc, "rules");
    for (size_t i = 0; i < cfg->nfc_rule_count; ++i) {
        const app_nfc_rule_t *rule = &cfg->nfc_rules[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "data", rule->data);
        cJSON_AddStringToObject(item, "name", rule->name);
        cJSON_AddNumberToObject(item, "opto12_pulses", rule->opto12_pulses);
        cJSON_AddBoolToObject(item, "open_relay4", rule->open_relay4);
        cJSON_AddItemToArray(rules, item);
    }

    cJSON *radar = cJSON_AddObjectToObject(root, "radar");
    cJSON_AddBoolToObject(radar, "enabled", cfg->radar.enabled);
    cJSON_AddNumberToObject(radar, "active_level", cfg->radar.active_level);
    cJSON_AddNumberToObject(radar, "trigger_delay_ms", cfg->radar.trigger_delay_ms);
    cJSON_AddNumberToObject(radar, "cycle_window_ms", cfg->radar.cycle_window_ms);
    cJSON_AddNumberToObject(radar, "opto12_pulses", cfg->radar.opto12_pulses);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

static esp_err_t save_to_nvs(const app_config_t *cfg)
{
    char *json = create_json_text(cfg);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(json);
        return err;
    }

    err = nvs_set_str(handle, APP_CONFIG_NVS_KEY_JSON, json);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    free(json);
    return err;
}

static esp_err_t save_to_sd(const app_config_t *cfg)
{
    if (!storage_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[64];
    int len = snprintf(path, sizeof(path), "%s%s", storage_sd_mount_point(), APP_CONFIG_SD_FILENAME);
    if (len < 0 || len >= (int)sizeof(path)) {
        return ESP_FAIL;
    }

    char *json = create_json_text(cfg);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool ok = save_json_file(path, json);
    free(json);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t load_from_nvs(app_config_t *cfg)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required = 0;
    err = nvs_get_str(handle, APP_CONFIG_NVS_KEY_JSON, NULL, &required);
    if (err != ESP_OK || required == 0) {
        nvs_close(handle);
        return err;
    }
    if (required > APP_CONFIG_MAX_JSON_SIZE) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    char *json = calloc(1, required);
    if (json == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(handle, APP_CONFIG_NVS_KEY_JSON, json, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(json);
        return err;
    }

    bool ok = load_from_json_text(cfg, json);
    free(json);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t load_from_sd(app_config_t *cfg)
{
    if (!storage_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[64];
    int len = snprintf(path, sizeof(path), "%s%s", storage_sd_mount_point(), APP_CONFIG_SD_FILENAME);
    if (len < 0 || len >= (int)sizeof(path)) {
        return ESP_FAIL;
    }

    char *json = calloc(1, APP_CONFIG_MAX_JSON_SIZE);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool ok = load_json_file(path, json, APP_CONFIG_MAX_JSON_SIZE);
    if (!ok) {
        free(json);
        return ESP_ERR_NOT_FOUND;
    }
    ok = load_from_json_text(cfg, json);
    free(json);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t app_config_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    app_config_set_defaults(&s_config);

    esp_err_t err = load_from_sd(&s_config);
    if (err == ESP_OK) {
        s_config.source = APP_CONFIG_SOURCE_SD;
        ESP_LOGI(TAG, "configuration loaded from SD");
        ESP_ERROR_CHECK_WITHOUT_ABORT(save_to_nvs(&s_config));
        s_inited = true;
        return ESP_OK;
    }

    app_config_set_defaults(&s_config);
    err = load_from_nvs(&s_config);
    if (err == ESP_OK) {
        s_config.source = APP_CONFIG_SOURCE_NVS;
        ESP_LOGI(TAG, "configuration loaded from NVS");
        s_inited = true;
        return ESP_OK;
    }

    app_config_set_defaults(&s_config);
    s_config.source = APP_CONFIG_SOURCE_DEFAULT;
    ESP_LOGW(TAG, "using built-in default configuration");
    ESP_ERROR_CHECK_WITHOUT_ABORT(save_to_nvs(&s_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(save_to_sd(&s_config));
    s_inited = true;
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    if (!s_inited) {
        ESP_ERROR_CHECK(app_config_init());
    }
    return &s_config;
}

void app_config_copy(app_config_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!s_inited) {
        ESP_ERROR_CHECK(app_config_init());
    }
    *out = s_config;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *cfg;
    s_config.version = APP_CONFIG_VERSION;

    bool sd_mounted = storage_sd_is_mounted();
    esp_err_t err_sd = save_to_sd(&s_config);
    esp_err_t err_nvs = save_to_nvs(&s_config);
    s_config.source = (err_sd == ESP_OK) ? APP_CONFIG_SOURCE_SD : APP_CONFIG_SOURCE_NVS;

    if (err_sd != ESP_OK) {
        ESP_LOGW(TAG, "save to SD failed: %s", esp_err_to_name(err_sd));
    }
    if (err_nvs != ESP_OK) {
        ESP_LOGW(TAG, "save to NVS failed: %s", esp_err_to_name(err_nvs));
    }
    if (sd_mounted && err_sd != ESP_OK) {
        return err_sd;
    }
    if (err_nvs != ESP_OK) {
        return err_nvs;
    }
    return ESP_OK;
}

static bool is_hex_ascii(uint8_t value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

esp_err_t app_config_find_nfc_action(uint8_t data0, uint8_t data1, app_nfc_rule_t *rule)
{
    if (!s_inited) {
        ESP_RETURN_ON_ERROR(app_config_init(), TAG, "app_config_init failed");
    }

    char data_text[3] = {0};
    if (is_hex_ascii(data0) && is_hex_ascii(data1)) {
        data_text[0] = (char)toupper((unsigned char)data0);
        data_text[1] = (char)toupper((unsigned char)data1);
    } else if (data0 == 0x00 && data1 <= 0x0F) {
        snprintf(data_text, sizeof(data_text), "%02X", data1);
    } else {
        snprintf(data_text, sizeof(data_text), "%02X", data0);
    }

    if (rule != NULL) {
        memset(rule, 0, sizeof(*rule));
    }
    for (size_t i = 0; i < s_config.nfc_rule_count; ++i) {
        if (strncmp(s_config.nfc_rules[i].data, data_text, 2) == 0) {
            if (rule != NULL) {
                *rule = s_config.nfc_rules[i];
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const char *app_config_source_name(app_config_source_t source)
{
    switch (source) {
        case APP_CONFIG_SOURCE_SD:
            return "SD";
        case APP_CONFIG_SOURCE_NVS:
            return "NVS";
        default:
            return "DEFAULT";
    }
}
