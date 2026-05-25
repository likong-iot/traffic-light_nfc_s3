#include "web_config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
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
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "ota_update.h"
#include "storage_sd.h"
#include "time_sync.h"

static const char *TAG = "web_config";

#define WEB_CONFIG_MAX_STA_CONN       4
#define WEB_CONFIG_HTTP_STACK_SIZE    8192
#define WEB_CONFIG_HTML_BUFFER_SIZE   18000
#define WEB_CONFIG_BODY_MAX_SIZE      8192

static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_ap_ssid[33] = {0};
static char s_ap_password[64] = {0};
static bool s_started = false;

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

static void html_escape(const char *src, char *dst, size_t dst_len)
{
    if (src == NULL || dst == NULL || dst_len == 0) {
        return;
    }
    size_t pos = 0;
    while (*src != '\0' && pos < dst_len - 1) {
        const char *esc = NULL;
        switch (*src) {
        case '<': esc = "&lt;"; break;
        case '>': esc = "&gt;"; break;
        case '&': esc = "&amp;"; break;
        case '"': esc = "&quot;"; break;
        case '\'': esc = "&#39;"; break;
        default: break;
        }
        if (esc != NULL) {
            size_t elen = strlen(esc);
            if (pos + elen >= dst_len) break;
            memcpy(dst + pos, esc, elen);
            pos += elen;
        } else {
            dst[pos++] = *src;
        }
        src++;
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

static int append_nfc_rows(char *html, size_t html_len, size_t *pos, const app_config_t *cfg)
{
    for (size_t i = 0; i < APP_CONFIG_MAX_NFC_RULES; ++i) {
        const app_nfc_rule_t empty = {0};
        const app_nfc_rule_t *rule = i < cfg->nfc_rule_count ? &cfg->nfc_rules[i] : &empty;
        char data_key[16];
        char name_key[16];
        char pulse_key[16];
        char esc_data[16] = {0};
        char esc_name[200] = {0};
        snprintf(data_key, sizeof(data_key), "nfc%u_data", (unsigned)i);
        snprintf(name_key, sizeof(name_key), "nfc%u_name", (unsigned)i);
        snprintf(pulse_key, sizeof(pulse_key), "nfc%u_pulses", (unsigned)i);
        html_escape(rule->data, esc_data, sizeof(esc_data));
        html_escape(rule->name, esc_name, sizeof(esc_name));

        const char *action_text = "仅映射 data 和 OPTO1+OPTO2 脉冲";
        const char *timing_name = NULL;
        int timing_value = 0;
        int timing_max = 600000;
        switch (i) {
        case 0:
            action_text = "1类卡：继电器4闭合，LED1点亮";
            timing_name = "class1_led1_hold_ms";
            timing_value = cfg->timing.class1_led1_hold_ms;
            break;
        case 1:
            action_text = "2类卡：继电器4闭合，LED1点亮";
            timing_name = "class2_led1_hold_ms";
            timing_value = cfg->timing.class2_led1_hold_ms;
            break;
        case 2:
            action_text = "3类卡：继电器4闭合，LED2等待，结束后补1个脉冲";
            timing_name = "class3_led2_hold_ms";
            timing_value = cfg->timing.class3_led2_hold_ms;
            timing_max = 3600000;
            break;
        case 3:
            action_text = "4类卡：LED2常亮，刷1类卡关闭";
            break;
        case 4:
            action_text = "5类卡：继电器4断开，LED2常亮，取消3类尾脉冲";
            break;
        case 5:
            action_text = "6类卡：LED2常亮，刷1类卡关闭";
            break;
        default:
            break;
        }

        if (timing_name != NULL) {
            if (append_html(html, html_len, pos,
                        "<div class='nfc-row'>"
                        "<input name='%s' value='%s' maxlength='2' placeholder='00'>"
                        "<input name='%s' value='%s' maxlength='31' placeholder='例如：1类卡'>"
                        "<input name='%s' type='number' min='1' max='20' value='%d'>"
                        "<div class='actioncell'><span>%s</span><label class='mini'>时长(ms)<input name='%s' type='number' min='0' max='%d' value='%d'></label></div>"
                        "</div>",
                        data_key, esc_data,
                        name_key, esc_name,
                        pulse_key, rule->opto12_pulses > 0 ? rule->opto12_pulses : 1,
                        action_text,
                        timing_name, timing_max, timing_value) != 0) {
                return -1;
            }
        } else {
            if (append_html(html, html_len, pos,
                        "<div class='nfc-row'>"
                        "<input name='%s' value='%s' maxlength='2' placeholder='00'>"
                        "<input name='%s' value='%s' maxlength='31' placeholder='例如：1类卡'>"
                        "<input name='%s' type='number' min='1' max='20' value='%d'>"
                        "<div class='actioncell'><span>%s</span></div>"
                        "</div>",
                        data_key, esc_data,
                        name_key, esc_name,
                        pulse_key, rule->opto12_pulses > 0 ? rule->opto12_pulses : 1,
                        action_text) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static esp_err_t send_config_page(httpd_req_t *req, const char *message)
{
    app_config_t cfg;
    app_config_copy(&cfg);

    char r1_start[6] = {0};
    char r1_end[6] = {0};
    char r2_start[6] = {0};
    char r2_end[6] = {0};
    char board_time[6] = {0};
    format_time_minutes(cfg.schedule.relay1_start_min, r1_start, sizeof(r1_start));
    format_time_minutes(cfg.schedule.relay1_end_min, r1_end, sizeof(r1_end));
    format_time_minutes(cfg.schedule.relay2_start_min, r2_start, sizeof(r2_start));
    format_time_minutes(cfg.schedule.relay2_end_min, r2_end, sizeof(r2_end));
    format_board_time(board_time, sizeof(board_time));

    char esc_ssid[200] = {0};
    char esc_password[400] = {0};
    html_escape(cfg.ap.ssid, esc_ssid, sizeof(esc_ssid));
    html_escape(cfg.ap.password, esc_password, sizeof(esc_password));

    char *html = malloc(WEB_CONFIG_HTML_BUFFER_SIZE);
    if (html == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }

    size_t pos = 0;
    int ok = 0;
    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<!doctype html><html lang='zh-CN'><head>"
                      "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                      "<title>NFC路灯控制器配置</title>"
                      "<style>"
                      "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin:0;background:#f4f6f8;color:#1f2933}"
                      ".layout{min-height:100vh}.side{position:fixed;left:0;top:0;bottom:0;width:180px;box-sizing:border-box;background:#14213d;color:#fff;padding:22px 16px;overflow-y:auto}.side h1{font-size:18px;margin:0 0 22px}.side a{display:block;color:#dbeafe;text-decoration:none;padding:10px 8px;border-radius:6px}.side a:hover{background:#263b65}.main{max-width:920px;margin-left:180px;padding:26px}.card{background:#fff;border:1px solid #d9dee7;border-radius:10px;padding:18px;margin-bottom:16px;box-shadow:0 1px 2px rgba(16,24,40,.04)}"
                      "h2{margin:0 0 14px;font-size:22px}h3{margin:14px 0 10px}.row{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:12px 0}.nfc-head,.nfc-row{display:grid;grid-template-columns:80px 1.1fr 100px 1.7fr;gap:10px;align-items:center;margin:8px 0}.nfc-head{font-size:13px;color:#66788a}.actioncell{font-size:14px;color:#334e68;line-height:1.5}.mini{margin-top:6px;font-size:13px}.mini input{margin-top:4px}.timebox{font-size:18px}.timebox span{font-weight:700}.msg{background:#e3f8e8;color:#176b37;padding:10px 12px;border-radius:6px;margin-bottom:14px}.warn{background:#fff7e6;color:#8a4b00;padding:10px 12px;border-radius:6px}.hint{font-size:13px;color:#66788a;line-height:1.6}.check{font-size:14px;color:#334e68}label{display:block;font-size:14px;color:#52606d;margin-bottom:6px}input{width:100%%;box-sizing:border-box;font-size:16px;padding:9px;border:1px solid #cbd2d9;border-radius:6px}input[type=checkbox]{width:auto}.btn{max-width:240px;margin-top:10px;padding:12px;font-size:17px;border:0;border-radius:6px;background:#0b6bcb;color:#fff}"
                      "@media(max-width:760px){.side{position:sticky;top:0;bottom:auto;width:auto;max-height:45vh;z-index:1}.side a{display:inline-block}.main{margin-left:0;padding:16px}.row,.nfc-head,.nfc-row{grid-template-columns:1fr}}"
                      "</style></head><body><div class='layout'><nav class='side'><h1>NFC路灯控制器</h1><a href='#time'>时间配置</a><a href='#ap'>AP配置</a><a href='#nfc'>NFC动作</a><a href='#radar'>雷达输入</a><a href='#log'>NFC日志</a><a href='#status'>系统状态</a><a href='#ota'>固件升级</a></nav><main class='main'>");

    if (message != NULL && message[0] != '\0') {
        ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos, "<div class='msg'>%s</div>", message);
    }

    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<form method='post' action='/save'>"
                      "<section id='time' class='card'><h2>时间配置</h2>"
                      "<div class='timebox'>控制器当前时间：<span id='board-time'>%s</span></div>"
                      "<p class='hint'>继电器1和继电器2按这里的时间闭合；支持跨天时间段，例如 18:00 到 07:00。</p>"
                      "<h3>继电器1</h3><div class='row'><div><label>闭合开始</label><input type='time' name='r1_start' value='%s' required></div><div><label>闭合结束</label><input type='time' name='r1_end' value='%s' required></div></div>"
                      "<h3>继电器2</h3><div class='row'><div><label>闭合开始</label><input type='time' name='r2_start' value='%s' required></div><div><label>闭合结束</label><input type='time' name='r2_end' value='%s' required></div></div>"
                      "</section>",
                      board_time, r1_start, r1_end, r2_start, r2_end);

    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<section id='ap' class='card'><h2>AP配置</h2>"
                      "<p class='hint'>AP 名称最长 32 字节，密码长度 8~63 字节。保存后写入配置文件和 NVS；当前连接可能仍保持旧 AP，重启后一定使用新配置。</p>"
                      "<div class='row'><div><label>AP 名称</label><input type='text' name='ap_ssid' value='%s' maxlength='32' required></div><div><label>AP 密码</label><input type='text' name='ap_password' value='%s' maxlength='63' minlength='8' required></div></div>"
                      "</section>",
                      esc_ssid, esc_password);

    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<section id='nfc' class='card'><h2>NFC动作</h2>"
                      "<p class='hint'>只匹配卡内 data[0..1]，不匹配 UID。00..05 固定对应 1..6 类卡；名称、OPTO1+OPTO2 脉冲数和相关动作时长在同一行配置。</p>"
                      "<div class='nfc-head'><span>data</span><span>名称</span><span>脉冲数</span><span>动作/时长</span></div>");
    ok |= append_nfc_rows(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos, &cfg);
    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos, "</section>");

    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<section id='radar' class='card'><h2>雷达输入</h2>"
                      "<p class='hint'>IO_IN1(GPIO16) 和 IO_IN2(GPIO38) 接外部雷达 INT，高电平有效。任意一路触发后，两路共同进入同一个周期窗口；窗口内再次触发不会重复输出。</p>"
                      "<label class='check'><input type='checkbox' name='radar_enabled' value='1' %s> 启用雷达触发</label>"
                      "<div class='row'><div><label>有效电平</label><input type='number' name='radar_active' min='0' max='1' value='%d'></div><div><label>触发延时(ms)</label><input type='number' name='radar_delay' min='0' max='60000' value='%d'></div></div>"
                      "<div class='row'><div><label>雷达触发后禁止再次触发时间(ms)</label><input type='number' name='radar_window' min='1000' max='600000' value='%d'></div><div><label>连续干扰周期数</label><input type='number' name='radar_cycles' min='1' max='20' value='%d'></div></div>"
                      "<div class='row'><div><label>OPTO1+OPTO2脉冲数</label><input type='number' name='radar_pulses' min='1' max='20' value='%d'></div><div><label>雷达锁定时长(ms)</label><input type='number' name='radar_lockout_ms' min='0' max='3600000' value='%d'></div></div>"
                      "</section>",
                      cfg.radar.enabled ? "checked" : "",
                      cfg.radar.active_level,
                      cfg.radar.trigger_delay_ms,
                      cfg.radar.cycle_window_ms,
                      cfg.radar.interference_cycles,
                      cfg.radar.opto12_pulses,
                      cfg.radar.lockout_ms);

    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<section id='log' class='card'><h2>NFC 日志</h2>"
                      "<p class='hint'>启用后每小时将刷卡记录写入 SD 卡 /NFCLOG/ 目录，按日期分文件，保留 200 天。</p>"
                      "<label class='check'><input type='checkbox' name='log_enabled' value='1' %s> 启用 NFC 日志记录</label>"
                      "</section>",
                      cfg.log_enabled ? "checked" : "");

    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<section id='status' class='card'><h2>系统状态</h2>"
                      "<p>配置来源：<b>%s</b></p><p>SD卡：<b>%s</b></p><p>SoftAP：<b>%s</b>，密码：<b>%s</b></p>"
                      "<p class='warn'>配置保存时会同时写 SD 卡文件 <b>/CONFIG.JSN</b> 和 ESP32 NVS。没有 SD 卡时会至少保存到 NVS；下次插入 SD 卡后，SD 文件优先级最高。</p>"
                      "</section><button class='btn' type='submit'>保存全部配置</button></form>",
                      app_config_source_name(cfg.source),
                      storage_sd_is_mounted() ? "已挂载" : "未挂载",
                      s_ap_ssid,
                      s_ap_password);

    char esc_version[64] = {0};
    html_escape(ota_running_version(), esc_version, sizeof(esc_version));
    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<section id='ota' class='card'><h2>固件升级 (OTA)</h2>"
                      "<p>当前版本：<b id='ota_ver'>%s</b></p>"
                      "<p class='hint'>支持 http:// 和 https:// 链接。固件下载完成后会自动重启。</p>"
                      "<div class='row'><div style='grid-column:1/3'><label>固件 bin 链接</label><input type='url' id='ota_url' placeholder='http(s)://example.com/app.bin' maxlength='255'></div></div>"
                      "<button class='btn' type='button' id='ota_btn'>开始升级</button>"
                      "<div style='margin-top:14px'><progress id='ota_pb' value='0' max='100' style='width:100%%;height:18px'></progress><div id='ota_text' style='margin-top:6px;color:#52606d'>就绪</div></div>"
                      "</section>",
                      esc_version);

    ok |= append_html(html, WEB_CONFIG_HTML_BUFFER_SIZE, &pos,
                      "<script>"
                      "function updateTime(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){document.getElementById('board-time').textContent=s.time||'--:--'}).catch(function(){document.getElementById('board-time').textContent='--:--'})}"
                      "setInterval(updateTime,5000);updateTime();"
                      "var otaPolling=false;"
                      "function pollOta(){fetch('/ota/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){"
                      "var pb=document.getElementById('ota_pb');var t=document.getElementById('ota_text');"
                      "var pct=s.total>0?Math.floor(s.downloaded*100/s.total):0;"
                      "pb.value=pct;"
                      "t.textContent=s.state+'  '+pct+'%  '+(s.message||'')+(s.total>0?'  ('+s.downloaded+'/'+s.total+')':'');"
                      "if(s.state==='success'||s.state==='failed'||s.state==='idle'){otaPolling=false;}else{setTimeout(pollOta,1000);}"
                      "}).catch(function(){if(otaPolling)setTimeout(pollOta,2000);});}"
                      "document.getElementById('ota_btn').onclick=function(){"
                      "var url=document.getElementById('ota_url').value.trim();"
                      "if(!url){alert('请输入 OTA 链接');return;}"
                      "if(!/^https?:\\/\\//.test(url)){alert('链接必须以 http:// 或 https:// 开头');return;}"
                      "if(!confirm('确认升级到: '+url+' ?'))return;"
                      "var fd=new FormData();fd.append('url',url);"
                      "fetch('/ota/start',{method:'POST',body:fd}).then(function(r){return r.json()}).then(function(s){"
                      "if(s.ok){otaPolling=true;pollOta();}else{alert('启动失败: '+s.error);}"
                      "}).catch(function(e){alert('请求失败: '+e);});"
                      "};"
                      "pollOta();"
                      "</script>"
                      "</main></div></body></html>");

    if (ok != 0) {
        free(html);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "html too large");
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
        return ESP_FAIL;
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
    if (req->content_len <= 0 || req->content_len > WEB_CONFIG_BODY_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_ERR_INVALID_ARG;
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    app_config_t cfg;
    app_config_copy(&cfg);
    if (!parse_post_config(body, &cfg)) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad config");
        return ESP_ERR_INVALID_ARG;
    }
    free(body);

    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save app config failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return err;
    }

    ESP_LOGI(TAG, "config saved: AP=%s NFC rules=%u timing=[%d/%d/%d] radar=%s delay=%dms window=%dms cycles=%d lockout=%dms",
             cfg.ap.ssid,
             (unsigned)cfg.nfc_rule_count,
             cfg.timing.class1_led1_hold_ms,
             cfg.timing.class2_led1_hold_ms,
             cfg.timing.class3_led2_hold_ms,
             cfg.radar.enabled ? "enabled" : "disabled",
             cfg.radar.trigger_delay_ms,
             cfg.radar.cycle_window_ms,
             cfg.radar.interference_cycles,
             cfg.radar.lockout_ms);
    return send_config_page(req, "配置已保存：已写入 SD 卡（如已挂载）和 ESP32 NVS。");
}

static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
    ota_status_t st = {0};
    ota_update_get_status(&st);

    char esc_msg[256] = {0};
    html_escape(st.message, esc_msg, sizeof(esc_msg));

    char body[512];
    int len = snprintf(body, sizeof(body),
                       "{\"state\":\"%s\",\"downloaded\":%d,\"total\":%d,\"message\":\"%s\"}",
                       ota_state_name(st.state),
                       st.bytes_downloaded,
                       st.bytes_total,
                       esc_msg);
    if (len < 0 || len >= (int)sizeof(body)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_start_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid body\"}", HTTPD_RESP_USE_STRLEN);
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no memory\"}", HTTPD_RESP_USE_STRLEN);
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv failed\"}", HTTPD_RESP_USE_STRLEN);
        }
        received += ret;
    }
    body[received] = '\0';

    char url[256] = {0};
    bool got_url = form_get_value(body, "url", url, sizeof(url));
    free(body);

    if (!got_url || url[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing url\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = ota_update_start(url);
    if (err != ESP_OK) {
        char body_resp[160];
        snprintf(body_resp, sizeof(body_resp),
                 "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, body_resp, HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "OTA started: url=%s", url);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = WEB_CONFIG_HTTP_STACK_SIZE;
    config.lru_purge_enable = true;
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
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &status), TAG, "register /status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save), TAG, "register /save failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &ota_status), TAG, "register /ota/status failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &ota_start), TAG, "register /ota/start failed");
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
    ESP_RETURN_ON_ERROR(start_softap(), TAG, "start_softap failed");
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "start_http_server failed");

    s_started = true;
    return ESP_OK;
}
