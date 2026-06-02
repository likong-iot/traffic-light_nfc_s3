#include "web_config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "ota_update.h"
#include "storage_sd.h"
#include "time_sync.h"

static const char *TAG = "web_config";

#define WEB_CONFIG_MAX_STA_CONN       4
#define WEB_CONFIG_HTTP_STACK_SIZE    8192
#define WEB_CONFIG_BODY_MAX_SIZE      8192
#define WEB_AUTH_COOKIE_NAME           "tl_session"
#define WEB_AUTH_TOKEN_LEN             16

static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_ap_ssid[33] = {0};
static char s_ap_password[64] = {0};
static bool s_started = false;
static char s_auth_token[WEB_AUTH_TOKEN_LEN + 1] = {0};

extern const uint8_t web_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t web_login_html_start[] asm("_binary_login_html_start");
extern const uint8_t web_login_html_end[] asm("_binary_login_html_end");
extern const uint8_t web_style_css_start[] asm("_binary_style_css_start");
extern const uint8_t web_style_css_end[] asm("_binary_style_css_end");
extern const uint8_t web_app_js_start[] asm("_binary_app_js_start");
extern const uint8_t web_app_js_end[] asm("_binary_app_js_end");
extern const uint8_t web_login_js_start[] asm("_binary_login_js_start");
extern const uint8_t web_login_js_end[] asm("_binary_login_js_end");

static bool parse_time_minutes(const char *text, int *minutes)
{
    if (text == NULL || minutes == NULL) {
        return false;
    }

    int hour = -1;
    int minute = -1;
    if (sscanf(text, "%d:%d", &hour, &minute) != 2) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }

    *minutes = hour * 60 + minute;
    return true;
}

static void format_board_time(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    if (!time_sync_is_synced()) {
        strlcpy(out, "--:--", out_len);
        return;
    }

    time_t now = 0;
    time(&now);

    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);
    snprintf(out, out_len, "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
}

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    if (src == NULL || dst == NULL || dst_len == 0) {
        return;
    }
    size_t pos = 0;
    while (*src != '\0' && pos < dst_len - 1) {
        unsigned char c = (unsigned char)*src++;
        const char *esc = NULL;
        char tmp[7] = {0};
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                esc = tmp;
            }
            break;
        }
        if (esc != NULL) {
            size_t elen = strlen(esc);
            if (pos + elen >= dst_len) {
                break;
            }
            memcpy(dst + pos, esc, elen);
            pos += elen;
        } else {
            dst[pos++] = (char)c;
        }
    }
    dst[pos] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *text)
{
    char *src = text;
    char *dst = text;

    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0) {
            *dst++ = (char)((hex_value(src[1]) << 4) | hex_value(src[2]));
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool form_get_value(char *body, const char *key, char *out, size_t out_len)
{
    if (body == NULL || key == NULL || out == NULL || out_len == 0) {
        return false;
    }

    size_t key_len = strlen(key);
    char *field = body;
    while (field != NULL && *field != '\0') {
        char *next = strchr(field, '&');
        if (next != NULL) {
            *next = '\0';
        }

        char *eq = strchr(field, '=');
        if (eq != NULL && (size_t)(eq - field) == key_len && memcmp(field, key, key_len) == 0) {
            strlcpy(out, eq + 1, out_len);
            url_decode(out);
            if (next != NULL) {
                *next = '&';
            }
            return true;
        }

        if (next == NULL) {
            break;
        }
        *next = '&';
        field = next + 1;
    }
    return false;
}

static bool parse_int_range(const char *text, int min_value, int max_value, int *out)
{
    if (text == NULL || out == NULL || *text == '\0') {
        return false;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < min_value || value > max_value) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool valid_ap_text(const char *ssid, const char *password)
{
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    size_t password_len = password ? strlen(password) : 0;
    return ssid_len > 0 && ssid_len <= 32 && password_len >= 8 && password_len <= 63;
}

static bool parse_hex_pair(const char *text, char out[3])
{
    if (text == NULL || out == NULL || strlen(text) != 2) {
        return false;
    }
    if (hex_value(text[0]) < 0 || hex_value(text[1]) < 0) {
        return false;
    }
    out[0] = (char)toupper((unsigned char)text[0]);
    out[1] = (char)toupper((unsigned char)text[1]);
    out[2] = '\0';
    return true;
}

static int append_html(char *html, size_t html_len, size_t *pos, const char *fmt, ...)
{
    if (*pos >= html_len) {
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(html + *pos, html_len - *pos, fmt, args);
    va_end(args);
    if (written < 0 || written >= (int)(html_len - *pos)) {
        return -1;
    }
    *pos += (size_t)written;
    return 0;
}

static esp_err_t send_embedded_file(httpd_req_t *req, const uint8_t *start, const uint8_t *end, const char *content_type)
{
    size_t len = (size_t)(end - start);
    if (len > 0 && start[len - 1] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)start, len);
}

static void auth_generate_token(void)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    for (size_t i = 0; i < 8; ++i) {
        s_auth_token[i] = hex[(a >> ((7 - i) * 4)) & 0x0F];
        s_auth_token[i + 8] = hex[(b >> ((7 - i) * 4)) & 0x0F];
    }
    s_auth_token[WEB_AUTH_TOKEN_LEN] = '\0';
}

static bool auth_password_matches(const char *password)
{
    if (password == NULL || password[0] == '\0') {
        return false;
    }

    app_config_t cfg;
    app_config_copy(&cfg);
    if (strcmp(password, s_ap_password) == 0) {
        return true;
    }
    if (cfg.ap.password[0] != '\0' && strcmp(password, cfg.ap.password) == 0) {
        return true;
    }
    return false;
}

static bool request_is_authenticated(httpd_req_t *req)
{
    if (s_auth_token[0] == '\0') {
        return false;
    }

    char cookie[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }

    char expected[64] = {0};
    snprintf(expected, sizeof(expected), WEB_AUTH_COOKIE_NAME "=%s", s_auth_token);
    return strstr(cookie, expected) != NULL;
}

static bool request_is_xhr(httpd_req_t *req)
{
    char requested_with[32] = {0};
    return httpd_req_get_hdr_value_str(req, "X-Requested-With", requested_with, sizeof(requested_with)) == ESP_OK &&
           strcmp(requested_with, "XMLHttpRequest") == 0;
}

static esp_err_t send_json_body(httpd_req_t *req, const char *status, const char *body)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *error)
{
    char escaped[96] = {0};
    json_escape(error != NULL ? error : "error", escaped, sizeof(escaped));

    char body[144];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    return send_json_body(req, status, body);
}

static esp_err_t send_json_ok(httpd_req_t *req)
{
    return send_json_body(req, NULL, "{\"ok\":true}");
}

static esp_err_t send_json_unauthorized(httpd_req_t *req)
{
    return send_json_error(req, "401 Unauthorized", "login required");
}

static bool require_auth_json(httpd_req_t *req)
{
    if (request_is_authenticated(req)) {
        return true;
    }

    char cookie[256] = {0};
    bool has_cookie = httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) == ESP_OK;
    ESP_LOGW(TAG, "HTTP %s rejected: login required (cookie=%s)", req->uri, has_cookie ? "present-but-invalid" : "missing");
    send_json_unauthorized(req);
    return false;
}

static esp_err_t redirect_to_login(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t send_config_page(httpd_req_t *req, const char *message)
{
    (void)message;
    if (!request_is_authenticated(req)) {
        return redirect_to_login(req);
    }
    return send_embedded_file(req, web_index_html_start, web_index_html_end, "text/html; charset=utf-8");
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_config_page(req, NULL);
}

static esp_err_t login_get_handler(httpd_req_t *req)
{
    return send_embedded_file(req, web_login_html_start, web_login_html_end, "text/html; charset=utf-8");
}

static esp_err_t style_get_handler(httpd_req_t *req)
{
    return send_embedded_file(req, web_style_css_start, web_style_css_end, "text/css; charset=utf-8");
}

static esp_err_t app_js_get_handler(httpd_req_t *req)
{
    return send_embedded_file(req, web_app_js_start, web_app_js_end, "application/javascript; charset=utf-8");
}

static esp_err_t login_js_get_handler(httpd_req_t *req)
{
    return send_embedded_file(req, web_login_js_start, web_login_js_end, "application/javascript; charset=utf-8");
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP POST /login received: body_len=%d", (int)req->content_len);
    if (req->content_len <= 0 || req->content_len > 256) {
        ESP_LOGW(TAG, "HTTP POST /login rejected: invalid body length=%d", (int)req->content_len);
        return send_json_error(req, NULL, "invalid body");
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        return send_json_error(req, NULL, "no memory");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return send_json_error(req, NULL, "recv failed");
        }
        received += ret;
    }
    body[received] = '\0';

    char password[80] = {0};
    bool got_password = form_get_value(body, "password", password, sizeof(password));
    free(body);

    if (!got_password || !auth_password_matches(password)) {
        ESP_LOGW(TAG, "HTTP POST /login rejected: password mismatch");
        return send_json_error(req, NULL, "password mismatch");
    }

    char cookie[96] = {0};
    snprintf(cookie, sizeof(cookie), WEB_AUTH_COOKIE_NAME "=%s; Path=/; Max-Age=86400; SameSite=Lax", s_auth_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    ESP_LOGI(TAG, "HTTP POST /login accepted");

    if (!request_is_xhr(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }

    return send_json_ok(req);
}

static esp_err_t logout_post_handler(httpd_req_t *req)
{
    (void)req;
    httpd_resp_set_hdr(req, "Set-Cookie", WEB_AUTH_COOKIE_NAME "=; Path=/; Max-Age=0; SameSite=Lax");
    ESP_LOGI(TAG, "HTTP POST /logout accepted");
    return send_json_ok(req);
}
static esp_err_t config_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET /config received");
    if (!require_auth_json(req)) {
        return ESP_OK;
    }

    app_config_t cfg;
    app_config_copy(&cfg);

    char *body = malloc(8192);
    if (body == NULL) {
        send_json_error(req, "500 Internal Server Error", "no memory");
        return ESP_ERR_NO_MEM;
    }

    char ap_ssid[80] = {0};
    char ap_password[96] = {0};
    char current_ssid[80] = {0};
    char current_password[96] = {0};
    char version[80] = {0};
    json_escape(cfg.ap.ssid, ap_ssid, sizeof(ap_ssid));
    json_escape(cfg.ap.password, ap_password, sizeof(ap_password));
    json_escape(s_ap_ssid, current_ssid, sizeof(current_ssid));
    json_escape(s_ap_password, current_password, sizeof(current_password));
    json_escape(ota_running_version(), version, sizeof(version));

    size_t pos = 0;
    int ok = 0;
    ok |= append_html(body, 8192, &pos,
                      "{\"version\":%d,\"config_source\":\"%s\",\"sd_mounted\":%s,"
                      "\"ota_version\":\"%s\","
                      "\"ap_current\":{\"ssid\":\"%s\",\"password\":\"%s\"},"
                      "\"ap\":{\"ssid\":\"%s\",\"password\":\"%s\"},"
                      "\"schedule\":{\"relay1_start_min\":%d,\"relay1_end_min\":%d,\"relay2_start_min\":%d,\"relay2_end_min\":%d},"
                      "\"timing\":{\"class1_led1_hold_ms\":%d,\"class2_led1_hold_ms\":%d,\"class3_led2_hold_ms\":%d},"
                      "\"radar\":{\"enabled\":%s,\"active_level\":%d,\"trigger_delay_ms\":%d,\"cycle_window_ms\":%d,\"high_level_threshold_ms\":%d,\"trigger_count_threshold\":%d,\"interference_cycles\":%d,\"opto12_pulses\":%d,\"lockout_ms\":%d},"
                      "\"log_enabled\":%s,\"nfc_rules\":[",
                      cfg.version,
                      app_config_source_name(cfg.source),
                      storage_sd_is_mounted() ? "true" : "false",
                      version,
                      current_ssid,
                      current_password,
                      ap_ssid,
                      ap_password,
                      cfg.schedule.relay1_start_min,
                      cfg.schedule.relay1_end_min,
                      cfg.schedule.relay2_start_min,
                      cfg.schedule.relay2_end_min,
                      cfg.timing.class1_led1_hold_ms,
                      cfg.timing.class2_led1_hold_ms,
                      cfg.timing.class3_led2_hold_ms,
                      cfg.radar.enabled ? "true" : "false",
                      cfg.radar.active_level,
                      cfg.radar.trigger_delay_ms,
                      cfg.radar.cycle_window_ms,
                      cfg.radar.high_level_threshold_ms,     // 新增参数
                      cfg.radar.trigger_count_threshold,     // 新增参数
                      cfg.radar.interference_cycles,
                      cfg.radar.opto12_pulses,
                      cfg.radar.lockout_ms,
                      cfg.log_enabled ? "true" : "false");

    for (size_t i = 0; i < cfg.nfc_rule_count && i < APP_CONFIG_MAX_NFC_RULES; ++i) {
        char data[16] = {0};
        char name[80] = {0};
        json_escape(cfg.nfc_rules[i].data, data, sizeof(data));
        json_escape(cfg.nfc_rules[i].name, name, sizeof(name));
        ok |= append_html(body, 8192, &pos,
                          "%s{\"data\":\"%s\",\"name\":\"%s\",\"opto12_pulses\":%d}",
                          i == 0 ? "" : ",",
                          data,
                          name,
                          cfg.nfc_rules[i].opto12_pulses);
    }
    ok |= append_html(body, 8192, &pos, "]}");

    if (ok != 0) {
        free(body);
        send_json_error(req, "500 Internal Server Error", "config too large");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP GET /config accepted: bytes=%u rules=%u", (unsigned)strlen(body), (unsigned)cfg.nfc_rule_count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!require_auth_json(req)) {
        return ESP_OK;
    }

    char time_text[6] = {0};
    format_board_time(time_text, sizeof(time_text));
    app_config_t cfg;
    app_config_copy(&cfg);

    char body[256];
    int len = snprintf(body, sizeof(body),
                       "{\"time\":\"%s\",\"synced\":%s,\"sd_mounted\":%s,\"config_source\":\"%s\"}",
                       time_text,
                       time_sync_is_synced() ? "true" : "false",
                       storage_sd_is_mounted() ? "true" : "false",
                       app_config_source_name(cfg.source));
    if (len < 0 || len >= (int)sizeof(body)) {
        return send_json_error(req, "500 Internal Server Error", "status response too large");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool parse_post_config(char *body, app_config_t *cfg)
{
    char value[64];
    char ap_ssid[64];
    char ap_password[64];
    int r1_start = 0;
    int r1_end = 0;
    int r2_start = 0;
    int r2_end = 0;
    int class1_led1_hold_ms = 0;
    int class2_led1_hold_ms = 0;
    int class3_led2_hold_ms = 0;

    if (!form_get_value(body, "r1_start", value, sizeof(value)) || !parse_time_minutes(value, &r1_start) ||
        !form_get_value(body, "r1_end", value, sizeof(value)) || !parse_time_minutes(value, &r1_end) ||
        !form_get_value(body, "r2_start", value, sizeof(value)) || !parse_time_minutes(value, &r2_start) ||
        !form_get_value(body, "r2_end", value, sizeof(value)) || !parse_time_minutes(value, &r2_end)) {
        return false;
    }

    cfg->schedule.relay1_start_min = r1_start;
    cfg->schedule.relay1_end_min = r1_end;
    cfg->schedule.relay2_start_min = r2_start;
    cfg->schedule.relay2_end_min = r2_end;

    if (!form_get_value(body, "class1_led1_hold_ms", value, sizeof(value)) ||
        !parse_int_range(value, 0, 600000, &class1_led1_hold_ms) ||
        !form_get_value(body, "class2_led1_hold_ms", value, sizeof(value)) ||
        !parse_int_range(value, 0, 600000, &class2_led1_hold_ms) ||
        !form_get_value(body, "class3_led2_hold_ms", value, sizeof(value)) ||
        !parse_int_range(value, 0, 3600000, &class3_led2_hold_ms)) {
        return false;
    }
    cfg->timing.class1_led1_hold_ms = class1_led1_hold_ms;
    cfg->timing.class2_led1_hold_ms = class2_led1_hold_ms;
    cfg->timing.class3_led2_hold_ms = class3_led2_hold_ms;

    if (!form_get_value(body, "ap_ssid", ap_ssid, sizeof(ap_ssid))) {
        return false;
    }
    if (!form_get_value(body, "ap_password", ap_password, sizeof(ap_password))) {
        return false;
    }
    if (!valid_ap_text(ap_ssid, ap_password)) {
        return false;
    }
    strlcpy(cfg->ap.ssid, ap_ssid, sizeof(cfg->ap.ssid));
    strlcpy(cfg->ap.password, ap_password, sizeof(cfg->ap.password));

    cfg->radar.enabled = form_get_value(body, "radar_enabled", value, sizeof(value));
    if (!form_get_value(body, "radar_active", value, sizeof(value)) || !parse_int_range(value, 0, 1, &cfg->radar.active_level) ||
        !form_get_value(body, "radar_delay", value, sizeof(value)) || !parse_int_range(value, 0, 60000, &cfg->radar.trigger_delay_ms) ||
        !form_get_value(body, "radar_window", value, sizeof(value)) || !parse_int_range(value, 1000, 600000, &cfg->radar.cycle_window_ms) ||
        !form_get_value(body, "radar_high_threshold", value, sizeof(value)) || !parse_int_range(value, 1000, 600000, &cfg->radar.high_level_threshold_ms) ||
        !form_get_value(body, "radar_count_threshold", value, sizeof(value)) || !parse_int_range(value, 1, 100, &cfg->radar.trigger_count_threshold) ||
        !form_get_value(body, "radar_cycles", value, sizeof(value)) || !parse_int_range(value, 1, 20, &cfg->radar.interference_cycles) ||
        !form_get_value(body, "radar_pulses", value, sizeof(value)) || !parse_int_range(value, 1, 20, &cfg->radar.opto12_pulses) ||
        !form_get_value(body, "radar_lockout_ms", value, sizeof(value)) || !parse_int_range(value, 0, 3600000, &cfg->radar.lockout_ms)) {
        return false;
    }

    cfg->log_enabled = form_get_value(body, "log_enabled", value, sizeof(value));

    size_t count = 0;
    for (size_t i = 0; i < APP_CONFIG_MAX_NFC_RULES; ++i) {
        char key[24];
        char data_text[16];
        snprintf(key, sizeof(key), "nfc%u_data", (unsigned)i);
        if (!form_get_value(body, key, data_text, sizeof(data_text)) || data_text[0] == '\0') {
            continue;
        }

        app_nfc_rule_t rule = {0};
        if (!parse_hex_pair(data_text, rule.data)) {
            return false;
        }

        snprintf(key, sizeof(key), "nfc%u_name", (unsigned)i);
        if (form_get_value(body, key, value, sizeof(value))) {
            strlcpy(rule.name, value, sizeof(rule.name));
        }
        if (rule.name[0] == '\0') {
            snprintf(rule.name, sizeof(rule.name), "NFC %s", rule.data);
        }

        snprintf(key, sizeof(key), "nfc%u_pulses", (unsigned)i);
        if (!form_get_value(body, key, value, sizeof(value)) || !parse_int_range(value, 1, 20, &rule.opto12_pulses)) {
            return false;
        }


        cfg->nfc_rules[count++] = rule;
    }

    cfg->nfc_rule_count = count;
    return count > 0;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (!require_auth_json(req)) {
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len > WEB_CONFIG_BODY_MAX_SIZE) {
        send_json_error(req, "400 Bad Request", "invalid body");
        return ESP_ERR_INVALID_ARG;
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        send_json_error(req, "500 Internal Server Error", "no memory");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            send_json_error(req, "500 Internal Server Error", "receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    app_config_t cfg;
    app_config_copy(&cfg);
    if (!parse_post_config(body, &cfg)) {
        free(body);
        send_json_error(req, "400 Bad Request", "bad config");
        return ESP_ERR_INVALID_ARG;
    }
    free(body);

    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save app config failed: %s", esp_err_to_name(err));
        send_json_error(req, "500 Internal Server Error", "save failed");
        return err;
    }

    ESP_LOGI(TAG, "config saved: AP=%s NFC rules=%u timing=[%d/%d/%d] radar=%s delay=%dms window=%dms high_threshold=%dms count_threshold=%d cycles=%d lockout=%dms",
             cfg.ap.ssid,
             (unsigned)cfg.nfc_rule_count,
             cfg.timing.class1_led1_hold_ms,
             cfg.timing.class2_led1_hold_ms,
             cfg.timing.class3_led2_hold_ms,
             cfg.radar.enabled ? "enabled" : "disabled",
             cfg.radar.trigger_delay_ms,
             cfg.radar.cycle_window_ms,
             cfg.radar.high_level_threshold_ms,
             cfg.radar.trigger_count_threshold,
             cfg.radar.interference_cycles,
             cfg.radar.lockout_ms);
    if (request_is_xhr(req)) {
        return send_json_ok(req);
    }
    return send_config_page(req, "配置已保存：已写入 SD 卡（如已挂载）和 ESP32 NVS。");
}

static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
    if (!require_auth_json(req)) {
        return ESP_OK;
    }

    ota_status_t st = {0};
    ota_update_get_status(&st);

    char esc_msg[256] = {0};
    json_escape(st.message, esc_msg, sizeof(esc_msg));

    char body[512];
    int len = snprintf(body, sizeof(body),
                       "{\"state\":\"%s\",\"downloaded\":%d,\"total\":%d,\"message\":\"%s\"}",
                       ota_state_name(st.state),
                       st.bytes_downloaded,
                       st.bytes_total,
                       esc_msg);
    if (len < 0 || len >= (int)sizeof(body)) {
        return send_json_error(req, "500 Internal Server Error", "ota status response too large");
    }
    if (st.state != OTA_STATE_IDLE) {
        ESP_LOGI(TAG, "HTTP GET /ota/status: state=%s downloaded=%d total=%d message=%s",
                 ota_state_name(st.state), st.bytes_downloaded, st.bytes_total, st.message);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_start_post_handler(httpd_req_t *req)
{
    if (!require_auth_json(req)) {
        return ESP_OK;
    }

    char content_type[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        strlcpy(content_type, "unknown", sizeof(content_type));
    }
    ESP_LOGI(TAG, "HTTP POST /ota/start received: body_len=%d content_type=%s",
             (int)req->content_len, content_type);

    if (req->content_len <= 0 || req->content_len > 1024) {
        ESP_LOGW(TAG, "HTTP POST /ota/start rejected: invalid body length=%d", (int)req->content_len);
        return send_json_error(req, NULL, "invalid body");
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        return send_json_error(req, NULL, "no memory");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return send_json_error(req, NULL, "recv failed");
        }
        received += ret;
    }
    body[received] = '\0';

    char url[256] = {0};
    bool got_url = form_get_value(body, "url", url, sizeof(url));
    free(body);

    if (!got_url || url[0] == '\0') {
        ESP_LOGW(TAG, "HTTP POST /ota/start rejected: missing form field 'url'");
        return send_json_error(req, NULL, "missing url");
    }

    esp_err_t err = ota_update_start(url);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP POST /ota/start rejected: ota_update_start failed err=%s url=%s",
                 esp_err_to_name(err), url);
        return send_json_error(req, NULL, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "HTTP POST /ota/start accepted: url=%s", url);
    return send_json_ok(req);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = WEB_CONFIG_HTTP_STACK_SIZE;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &config), TAG, "httpd_start failed");

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_uri_t index = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_uri_t login_get = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_get_handler,
    };
    httpd_uri_t login_html = {
        .uri = "/login.html",
        .method = HTTP_GET,
        .handler = login_get_handler,
    };
    httpd_uri_t style = {
        .uri = "/style.css",
        .method = HTTP_GET,
        .handler = style_get_handler,
    };
    httpd_uri_t app_js = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = app_js_get_handler,
    };
    httpd_uri_t login_js = {
        .uri = "/login.js",
        .method = HTTP_GET,
        .handler = login_js_get_handler,
    };
    httpd_uri_t login_post = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
    };
    httpd_uri_t logout_post = {
        .uri = "/logout",
        .method = HTTP_POST,
        .handler = logout_post_handler,
    };
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_uri_t config_get = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    httpd_uri_t ota_status = {
        .uri = "/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_get_handler,
    };
    httpd_uri_t ota_start = {
        .uri = "/ota/start",
        .method = HTTP_POST,
        .handler = ota_start_post_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &root), TAG, "register / failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &index), TAG, "register /index.html failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &login_get), TAG, "register GET /login failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &login_html), TAG, "register /login.html failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &style), TAG, "register /style.css failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &app_js), TAG, "register /app.js failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &login_js), TAG, "register /login.js failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &login_post), TAG, "register POST /login failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &logout_post), TAG, "register POST /logout failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &status), TAG, "register /status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &config_get), TAG, "register /config failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save), TAG, "register /save failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &ota_status), TAG, "register /ota/status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &ota_start), TAG, "register /ota/start failed");
    ESP_LOGI(TAG, "HTTP config server ready at http://192.168.4.1/ (login page: /login)");
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = (const wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station connected: " MACSTR " aid=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = (const wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station disconnected: " MACSTR " aid=%d reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

static esp_err_t start_softap(void)
{
    app_config_t cfg;
    app_config_copy(&cfg);
    if (cfg.ap.ssid[0] == '\0') {
        app_config_build_default_ap_name(s_ap_ssid, sizeof(s_ap_ssid));
    } else {
        strlcpy(s_ap_ssid, cfg.ap.ssid, sizeof(s_ap_ssid));
    }
    if (cfg.ap.password[0] == '\0') {
        strlcpy(s_ap_password, "12345678", sizeof(s_ap_password));
    } else {
        strlcpy(s_ap_password, cfg.ap.password, sizeof(s_ap_password));
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(s_ap_netif), TAG, "stop AP DHCP server failed");
    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip_info), TAG, "set AP IP failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "start AP DHCP server failed");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            wifi_event_handler,
                                                            NULL,
                                                            NULL),
                        TAG,
                        "register WiFi AP event handler failed");

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, s_ap_password, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = WEB_CONFIG_MAX_STA_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "esp_wifi_set_mode AP failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "esp_wifi_set_config AP failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    esp_netif_ip_info_t actual_ip = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_get_ip_info(s_ap_netif, &actual_ip));
    ESP_LOGI(TAG, "SoftAP started: SSID=%s password=%s IP=http://192.168.4.1/",
             s_ap_ssid, s_ap_password);
    ESP_LOGI(TAG, "SoftAP netif: ip=" IPSTR " gw=" IPSTR " mask=" IPSTR " DHCP server=on",
             IP2STR(&actual_ip.ip), IP2STR(&actual_ip.gw), IP2STR(&actual_ip.netmask));
    return ESP_OK;
}

esp_err_t web_config_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(app_config_init(), TAG, "app_config_init failed");
    ESP_RETURN_ON_ERROR(ota_update_init(), TAG, "ota_update_init failed");
    ESP_RETURN_ON_ERROR(start_softap(), TAG, "start_softap failed");
    auth_generate_token();
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "start_http_server failed");

    s_started = true;
    return ESP_OK;
}
