#include "web_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "time_sync.h"

static const char *TAG = "web_config";

#define WEB_CONFIG_AP_PREFIX          "traffic_light_"
#define WEB_CONFIG_AP_PASSWORD        "12345678"
#define WEB_CONFIG_MAX_STA_CONN       4
#define WEB_CONFIG_TASK_STACK         4096
#define WEB_CONFIG_TASK_PRIORITY      2
#define WEB_CONFIG_SCHEDULE_POLL_MS   5000
#define WEB_CONFIG_NVS_NAMESPACE      "relay_cfg"
#define WEB_CONFIG_DEFAULT_START_MIN  (18 * 60)
#define WEB_CONFIG_DEFAULT_END_MIN    (7 * 60)
#define WEB_CONFIG_HTTP_STACK_SIZE    8192
#define WEB_CONFIG_HTML_BUFFER_SIZE   4096

typedef struct {
    int relay1_start_min;
    int relay1_end_min;
    int relay2_start_min;
    int relay2_end_min;
} relay_schedule_config_t;

static relay_schedule_config_t s_config = {
    .relay1_start_min = WEB_CONFIG_DEFAULT_START_MIN,
    .relay1_end_min = WEB_CONFIG_DEFAULT_END_MIN,
    .relay2_start_min = WEB_CONFIG_DEFAULT_START_MIN,
    .relay2_end_min = WEB_CONFIG_DEFAULT_END_MIN,
};
static httpd_handle_t s_httpd = NULL;
static TaskHandle_t s_schedule_task = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_ap_ssid[33] = {0};
static bool s_started = false;
static bool s_relay_state_valid = false;
static bool s_last_relay1_closed = false;
static bool s_last_relay2_closed = false;

static esp_err_t save_config(void);
static void apply_relay_schedule_once(void);

static bool minute_in_window(int now_min, int start_min, int end_min)
{
    if (start_min == end_min) {
        return true;
    }
    if (start_min < end_min) {
        return now_min >= start_min && now_min < end_min;
    }
    return now_min >= start_min || now_min < end_min;
}

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

static void format_time_minutes(int minutes, char *out, size_t out_len)
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

static esp_err_t load_config(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEB_CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "relay schedule config not found; saving defaults 18:00-07:00 to NVS");
        return save_config();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open read failed");

    int32_t value = 0;
    if (nvs_get_i32(nvs, "r1_start", &value) == ESP_OK) {
        s_config.relay1_start_min = value;
    }
    if (nvs_get_i32(nvs, "r1_end", &value) == ESP_OK) {
        s_config.relay1_end_min = value;
    }
    if (nvs_get_i32(nvs, "r2_start", &value) == ESP_OK) {
        s_config.relay2_start_min = value;
    }
    if (nvs_get_i32(nvs, "r2_end", &value) == ESP_OK) {
        s_config.relay2_end_min = value;
    }
    nvs_close(nvs);

    ESP_LOGI(TAG, "relay schedule config loaded");
    return ESP_OK;
}

static esp_err_t save_config(void)
{
    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open(WEB_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open write failed");

    esp_err_t err = nvs_set_i32(nvs, "r1_start", s_config.relay1_start_min);
    if (err != ESP_OK) {
        nvs_close(nvs);
        ESP_LOGE(TAG, "NVS write r1_start failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(nvs, "r1_end", s_config.relay1_end_min);
    if (err != ESP_OK) {
        nvs_close(nvs);
        ESP_LOGE(TAG, "NVS write r1_end failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(nvs, "r2_start", s_config.relay2_start_min);
    if (err != ESP_OK) {
        nvs_close(nvs);
        ESP_LOGE(TAG, "NVS write r2_start failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(nvs, "r2_end", s_config.relay2_end_min);
    if (err != ESP_OK) {
        nvs_close(nvs);
        ESP_LOGE(TAG, "NVS write r2_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "relay schedule committed to NVS");
    } else {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t send_config_page(httpd_req_t *req, const char *message)
{
    char r1_start[6] = {0};
    char r1_end[6] = {0};
    char r2_start[6] = {0};
    char r2_end[6] = {0};
    char board_time[6] = {0};
    format_time_minutes(s_config.relay1_start_min, r1_start, sizeof(r1_start));
    format_time_minutes(s_config.relay1_end_min, r1_end, sizeof(r1_end));
    format_time_minutes(s_config.relay2_start_min, r2_start, sizeof(r2_start));
    format_time_minutes(s_config.relay2_end_min, r2_end, sizeof(r2_end));
    format_board_time(board_time, sizeof(board_time));

    char *html = malloc(WEB_CONFIG_HTML_BUFFER_SIZE);
    if (html == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }

    int len = snprintf(html, WEB_CONFIG_HTML_BUFFER_SIZE,
                       "<!doctype html><html lang=\"zh-CN\"><head>"
                       "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<title>Traffic Light Config</title>"
                       "<style>"
                       "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin:0;background:#f6f7f9;color:#1f2933}"
                       "main{max-width:560px;margin:0 auto;padding:24px 16px}"
                       "h1{font-size:22px;margin:0 0 16px}"
                       "form{background:#fff;border:1px solid #d9dee7;border-radius:8px;padding:18px}"
                       ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:14px 0}"
                       ".time{background:#fff;border:1px solid #d9dee7;border-radius:8px;padding:14px 18px;margin-bottom:14px;font-size:18px}"
                       ".time span{font-weight:700}"
                       "label{display:block;font-size:14px;color:#52606d;margin-bottom:6px}"
                       "input{width:100%%;box-sizing:border-box;font-size:18px;padding:10px;border:1px solid #cbd2d9;border-radius:6px}"
                       "button{width:100%%;margin-top:16px;padding:12px;font-size:18px;border:0;border-radius:6px;background:#0b6bcb;color:#fff}"
                       ".msg{padding:10px 12px;border-radius:6px;background:#e3f8e8;color:#176b37;margin:0 0 14px}"
                       ".hint{font-size:13px;color:#66788a;line-height:1.5;margin-top:16px}"
                       "</style></head><body><main>"
                       "<h1>继电器定时配置</h1>"
                       "<div class=\"time\">板子当前时间：<span id=\"board-time\">%s</span></div>"
                       "%s"
                       "<form method=\"post\" action=\"/save\">"
                       "<h2>继电器1</h2>"
                       "<div class=\"row\"><div><label>闭合开始</label><input type=\"time\" name=\"r1_start\" value=\"%s\" required></div>"
                       "<div><label>闭合结束</label><input type=\"time\" name=\"r1_end\" value=\"%s\" required></div></div>"
                       "<h2>继电器2</h2>"
                       "<div class=\"row\"><div><label>闭合开始</label><input type=\"time\" name=\"r2_start\" value=\"%s\" required></div>"
                       "<div><label>闭合结束</label><input type=\"time\" name=\"r2_end\" value=\"%s\" required></div></div>"
                       "<button type=\"submit\">保存</button>"
                       "<p class=\"hint\">默认规则为晚上18:00闭合，次日07:00断开。时间同步完成后，设备会按这里的时间控制继电器1和继电器2。</p>"
                       "</form>"
                       "<script>"
                       "function updateTime(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){document.getElementById('board-time').textContent=s.time||'--:--'}).catch(function(){document.getElementById('board-time').textContent='--:--'})}"
                       "setInterval(updateTime,5000);updateTime();"
                       "</script></main></body></html>",
                       board_time,
                       message ? message : "",
                       r1_start, r1_end, r2_start, r2_end);
    if (len < 0 || len >= WEB_CONFIG_HTML_BUFFER_SIZE) {
        free(html);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_config_page(req, NULL);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char board_time[6] = {0};
    format_board_time(board_time, sizeof(board_time));

    char body[256];
    int len = snprintf(body, sizeof(body),
                       "{"
                       "\"ok\":true,"
                       "\"ssid\":\"%s\","
                       "\"ip\":\"192.168.4.1\","
                       "\"time\":\"%s\","
                       "\"time_synced\":%s"
                       "}",
                       s_ap_ssid,
                       board_time,
                       time_sync_is_synced() ? "true" : "false");
    if (len < 0 || len >= (int)sizeof(body)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }

    char body[513] = {0};
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char r1_start_text[8] = {0};
    char r1_end_text[8] = {0};
    char r2_start_text[8] = {0};
    char r2_end_text[8] = {0};
    int r1_start = 0;
    int r1_end = 0;
    int r2_start = 0;
    int r2_end = 0;

    if (!form_get_value(body, "r1_start", r1_start_text, sizeof(r1_start_text)) ||
        !form_get_value(body, "r1_end", r1_end_text, sizeof(r1_end_text)) ||
        !form_get_value(body, "r2_start", r2_start_text, sizeof(r2_start_text)) ||
        !form_get_value(body, "r2_end", r2_end_text, sizeof(r2_end_text)) ||
        !parse_time_minutes(r1_start_text, &r1_start) ||
        !parse_time_minutes(r1_end_text, &r1_end) ||
        !parse_time_minutes(r2_start_text, &r2_start) ||
        !parse_time_minutes(r2_end_text, &r2_end)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad time");
        return ESP_FAIL;
    }

    s_config.relay1_start_min = r1_start;
    s_config.relay1_end_min = r1_end;
    s_config.relay2_start_min = r2_start;
    s_config.relay2_end_min = r2_end;

    esp_err_t err = save_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save relay schedule failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "relay schedule saved: R1 %s-%s, R2 %s-%s",
             r1_start_text, r1_end_text, r2_start_text, r2_end_text);
    apply_relay_schedule_once();
    return send_config_page(req, "<p class=\"msg\">已保存</p>");
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = WEB_CONFIG_HTTP_STACK_SIZE;
    config.uri_match_fn = httpd_uri_match_wildcard;

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
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &root), TAG, "register / failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &index), TAG, "register /index.html failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &status), TAG, "register /status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save), TAG, "register /save failed");
    ESP_LOGI(TAG, "HTTP config server ready at http://192.168.4.1/");
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
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "read WiFi SoftAP MAC failed");

    snprintf(s_ap_ssid, sizeof(s_ap_ssid), WEB_CONFIG_AP_PREFIX "%02X%02X", mac[4], mac[5]);

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
    strlcpy((char *)ap_cfg.ap.password, WEB_CONFIG_AP_PASSWORD, sizeof(ap_cfg.ap.password));
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
             s_ap_ssid, WEB_CONFIG_AP_PASSWORD);
    ESP_LOGI(TAG, "SoftAP netif: ip=" IPSTR " gw=" IPSTR " mask=" IPSTR " DHCP server=on",
             IP2STR(&actual_ip.ip), IP2STR(&actual_ip.gw), IP2STR(&actual_ip.netmask));
    return ESP_OK;
}

static void apply_relay_schedule_once(void)
{
    if (!time_sync_is_synced()) {
        ESP_LOGI(TAG, "[schedule] waiting for time sync before controlling relay1/relay2");
        return;
    }

    time_t now = 0;
    time(&now);

    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);
    int now_min = tm_info.tm_hour * 60 + tm_info.tm_min;

    bool relay1_closed = minute_in_window(now_min, s_config.relay1_start_min, s_config.relay1_end_min);
    bool relay2_closed = minute_in_window(now_min, s_config.relay2_start_min, s_config.relay2_end_min);
    bool changed = !s_relay_state_valid ||
                   relay1_closed != s_last_relay1_closed ||
                   relay2_closed != s_last_relay2_closed;

    if (!s_relay_state_valid || relay1_closed != s_last_relay1_closed) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(1, relay1_closed));
        s_last_relay1_closed = relay1_closed;
    }
    if (!s_relay_state_valid || relay2_closed != s_last_relay2_closed) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(board_hal_set_relay(2, relay2_closed));
        s_last_relay2_closed = relay2_closed;
    }
    if (!s_relay_state_valid) {
        s_relay_state_valid = true;
    }

    if (changed) {
        ESP_LOGI(TAG, "[schedule] %02d:%02d relay1=%s relay2=%s",
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 relay1_closed ? "closed" : "released",
                 relay2_closed ? "closed" : "released");
    }
}

static void schedule_task(void *arg)
{
    (void)arg;

    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_LOGI(TAG, "relay schedule task started; timezone=Asia/Shanghai");
    while (1) {
        apply_relay_schedule_once();
        vTaskDelay(pdMS_TO_TICKS(WEB_CONFIG_SCHEDULE_POLL_MS));
    }
}

esp_err_t web_config_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(load_config(), TAG, "load_config failed");
    ESP_RETURN_ON_ERROR(start_softap(), TAG, "start_softap failed");
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "start_http_server failed");

    BaseType_t ok = xTaskCreatePinnedToCore(schedule_task,
                                            "relay_sched",
                                            WEB_CONFIG_TASK_STACK,
                                            NULL,
                                            WEB_CONFIG_TASK_PRIORITY,
                                            &s_schedule_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}
