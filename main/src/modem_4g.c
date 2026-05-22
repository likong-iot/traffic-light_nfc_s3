#include "modem_4g.h"

#include <stdbool.h>
#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_modem.h"
#include "pin_map.h"

static const char *TAG = "modem_4g";

static bool s_connected = false;
static bool s_dte_up = false;
static bool s_cdc_installed = false;
static bool s_modem_installed = false;
static TaskHandle_t s_task_handle = NULL;

typedef enum {
    MODEM_BOOT_STAGE_NOT_STARTED = 0,
    MODEM_BOOT_STAGE_USB_HOST_READY,
    MODEM_BOOT_STAGE_USB_MODEM_INSTALLED,
    MODEM_BOOT_STAGE_USB_DEVICE_SEEN,
    MODEM_BOOT_STAGE_USB_ID_MATCHED,
    MODEM_BOOT_STAGE_USB_PORT_OPEN,
    MODEM_BOOT_STAGE_DTE_CONNECTED,
    MODEM_BOOT_STAGE_AT_PARSER_READY,
    MODEM_BOOT_STAGE_AT_OK,
    MODEM_BOOT_STAGE_SIM_READY,
    MODEM_BOOT_STAGE_SIGNAL_OK,
    MODEM_BOOT_STAGE_NETWORK_REGISTERED,
    MODEM_BOOT_STAGE_PPP_DIALING,
    MODEM_BOOT_STAGE_PPP_GOT_IP,
} modem_boot_stage_t;

static modem_boot_stage_t s_boot_stage = MODEM_BOOT_STAGE_NOT_STARTED;
static int32_t s_last_ppp_event = -1;

#define MODEM_TASK_STACK        8192
#define MODEM_TASK_PRIORITY     5
/* If USB hasn't been enumerated within this time, do a manual POWERKEY pulse. */
#define POWERKEY_KICK_DELAY_MS  12000
#define POWERKEY_ACTIVE_MS      1200
#define POWERKEY_SETTLE_MS      8000
#define BOOT_TIMEOUT_MS         120000
/* Interval between "still waiting" progress logs. */
#define WAIT_LOG_INTERVAL_MS    5000
#define USBH_CDC_TASK_STACK     4096
#define USBH_CDC_TASK_PRIORITY  5
#define MODEM_AT_BUFFER_SIZE    512
#define POWERKEY_INACTIVE_LEVEL 1

static const usb_modem_id_t s_usb_modem_id_list[] = {
    {.match_id = {USB_DEVICE_ID_MATCH_VID_PID, 0x19D1, 0x0001}, 2, -1, "Luat Air780E/AIR780ER"},
    {.match_id = {0}},
};

static const char *boot_stage_to_text(modem_boot_stage_t stage)
{
    switch (stage) {
    case MODEM_BOOT_STAGE_NOT_STARTED:
        return "not started";
    case MODEM_BOOT_STAGE_USB_HOST_READY:
        return "USB host driver installed; waiting for USB enumeration";
    case MODEM_BOOT_STAGE_USB_MODEM_INSTALLED:
        return "USB modem driver installed; waiting for USB device";
    case MODEM_BOOT_STAGE_USB_DEVICE_SEEN:
        return "USB device seen; checking VID/PID/interface match";
    case MODEM_BOOT_STAGE_USB_ID_MATCHED:
        return "USB VID/PID/interface matched; opening CDC ports";
    case MODEM_BOOT_STAGE_USB_PORT_OPEN:
        return "USB CDC port open; waiting for DTE connect";
    case MODEM_BOOT_STAGE_DTE_CONNECTED:
        return "DTE connected; waiting for AT parser start";
    case MODEM_BOOT_STAGE_AT_PARSER_READY:
        return "AT parser running; waiting for AT response";
    case MODEM_BOOT_STAGE_AT_OK:
        return "AT communication OK; checking SIM";
    case MODEM_BOOT_STAGE_SIM_READY:
        return "SIM ready; checking signal";
    case MODEM_BOOT_STAGE_SIGNAL_OK:
        return "signal OK; checking network registration";
    case MODEM_BOOT_STAGE_NETWORK_REGISTERED:
        return "network registered; waiting for PPP dial";
    case MODEM_BOOT_STAGE_PPP_DIALING:
        return "PPP dialing/negotiating; waiting for IP";
    case MODEM_BOOT_STAGE_PPP_GOT_IP:
        return "PPP got IP";
    default:
        return "unknown";
    }
}

static const char *boot_wait_phase_to_text(modem_boot_stage_t stage)
{
    switch (stage) {
    case MODEM_BOOT_STAGE_NOT_STARTED:
    case MODEM_BOOT_STAGE_USB_HOST_READY:
    case MODEM_BOOT_STAGE_USB_MODEM_INSTALLED:
        return "等枚举";
    case MODEM_BOOT_STAGE_USB_DEVICE_SEEN:
    case MODEM_BOOT_STAGE_USB_ID_MATCHED:
    case MODEM_BOOT_STAGE_USB_PORT_OPEN:
    case MODEM_BOOT_STAGE_DTE_CONNECTED:
    case MODEM_BOOT_STAGE_AT_PARSER_READY:
        return "等 AT";
    case MODEM_BOOT_STAGE_AT_OK:
    case MODEM_BOOT_STAGE_SIM_READY:
    case MODEM_BOOT_STAGE_SIGNAL_OK:
    case MODEM_BOOT_STAGE_NETWORK_REGISTERED:
        return "等 PPP";
    case MODEM_BOOT_STAGE_PPP_DIALING:
        return "等 IP";
    case MODEM_BOOT_STAGE_PPP_GOT_IP:
        return "已连接";
    default:
        return "等待中";
    }
}

static const char *pin_state_to_text(esp_modem_pin_state_t state)
{
    switch (state) {
    case PIN_READY:
        return "READY";
    case PIN_SIM_PIN:
        return "SIM PIN required";
    case PIN_SIM_PIN2:
        return "SIM PIN2 required";
    case PIN_SIM_PUK:
        return "SIM PUK required";
    case PIN_SIM_PUK2:
        return "SIM PUK2 required";
    case PIN_UNKNOWN:
    default:
        return "unknown / no SIM / not ready";
    }
}

static const char *cereg_stat_to_text(int stat)
{
    switch (stat) {
    case 0:
        return "not registered, not searching";
    case 1:
        return "registered, home network";
    case 2:
        return "searching / trying to register";
    case 3:
        return "registration denied";
    case 4:
        return "unknown";
    case 5:
        return "registered, roaming";
    default:
        return "unrecognized";
    }
}

static const char *ppp_event_to_text(int32_t event_id)
{
    switch (event_id) {
    case -1:
        return "no PPP status event yet";
    case NETIF_PPP_ERRORNONE:
        return "PPP no error";
    case NETIF_PPP_ERRORPARAM:
        return "PPP invalid parameter";
    case NETIF_PPP_ERROROPEN:
        return "PPP unable to open session";
    case NETIF_PPP_ERRORDEVICE:
        return "PPP invalid I/O device";
    case NETIF_PPP_ERRORALLOC:
        return "PPP allocation failed";
    case NETIF_PPP_ERRORUSER:
        return "PPP stopped by user";
    case NETIF_PPP_ERRORCONNECT:
        return "PPP connection lost";
    case NETIF_PPP_ERRORAUTHFAIL:
        return "PPP authentication failed";
    case NETIF_PPP_ERRORPROTOCOL:
        return "PPP protocol failed";
    case NETIF_PPP_ERRORPEERDEAD:
        return "PPP peer timeout";
    case NETIF_PPP_ERRORIDLETIMEOUT:
        return "PPP idle timeout";
    case NETIF_PPP_ERRORCONNECTTIME:
        return "PPP max connect time reached";
    case NETIF_PPP_ERRORLOOPBACK:
        return "PPP loopback detected";
    case NETIF_PPP_PHASE_DEAD:
        return "PPP phase dead";
    case NETIF_PPP_PHASE_HOLDOFF:
        return "PPP phase holdoff";
    case NETIF_PPP_PHASE_INITIALIZE:
        return "PPP phase initialize";
    case NETIF_PPP_PHASE_SERIALCONN:
        return "PPP phase serial connected";
    case NETIF_PPP_PHASE_ESTABLISH:
        return "PPP phase establish";
    case NETIF_PPP_PHASE_AUTHENTICATE:
        return "PPP phase authenticate";
    case NETIF_PPP_PHASE_NETWORK:
        return "PPP phase network";
    case NETIF_PPP_PHASE_RUNNING:
        return "PPP phase running";
    case NETIF_PPP_PHASE_TERMINATE:
        return "PPP phase terminate";
    case NETIF_PPP_PHASE_DISCONNECT:
        return "PPP phase disconnect";
    case NETIF_PPP_CONNECT_FAILED:
        return "PPP connect failed";
    default:
        return "PPP status event";
    }
}

static void set_boot_stage(modem_boot_stage_t stage)
{
    if (stage > s_boot_stage) {
        s_boot_stage = stage;
        ESP_LOGI(TAG, "  [diag] stage -> %s", boot_stage_to_text(stage));
    }
}

static void log_usb_modem_diag(const usb_modem_diag_t *diag)
{
    if (diag == NULL) {
        return;
    }

    if (!diag->device_seen) {
        ESP_LOGI(TAG, "  [diag] USB device: not enumerated yet");
        return;
    }

    ESP_LOGI(TAG,
             "  [diag] USB device: addr=%u VID=0x%04X PID=0x%04X bcd=0x%04X class=0x%02X/0x%02X/0x%02X cfg=%u interfaces=%u cdc_match_intf=%d",
             diag->dev_addr, diag->vid, diag->pid, diag->bcd_device,
             diag->device_class, diag->device_subclass, diag->device_protocol,
             diag->config_value, diag->config_interfaces, diag->cdc_matched_intf_num);
    ESP_LOGI(TAG,
             "  [diag] USB match: id_matched=%d name=%s modem_itf=%d present=%d open=%d at_itf=%d present=%d open=%d dte_connected=%d",
             diag->id_matched, diag->matched_name ? diag->matched_name : "(none)",
             diag->modem_itf_num, diag->modem_itf_present, diag->modem_port_open,
             diag->at_itf_num, diag->at_itf_present, diag->at_port_open, diag->dte_connected);
}

static bool poll_modem_diagnostics(bool verbose)
{
    esp_netif_t *ppp = usbh_modem_get_netif();
    at_handle_t at = usbh_modem_get_atparser();
    usb_modem_diag_t diag = {0};
    bool has_diag = usbh_modem_get_diag(&diag);

    if (ppp != NULL) {
        set_boot_stage(MODEM_BOOT_STAGE_USB_MODEM_INSTALLED);
    }

    if (has_diag) {
        if (diag.device_seen) {
            set_boot_stage(MODEM_BOOT_STAGE_USB_DEVICE_SEEN);
        }
        if (diag.id_matched) {
            set_boot_stage(MODEM_BOOT_STAGE_USB_ID_MATCHED);
        }
        if (diag.modem_port_open || diag.at_port_open) {
            set_boot_stage(MODEM_BOOT_STAGE_USB_PORT_OPEN);
        }
        if (diag.dte_connected) {
            set_boot_stage(MODEM_BOOT_STAGE_DTE_CONNECTED);
        }
        if (verbose) {
            log_usb_modem_diag(&diag);
        }
    } else if (verbose) {
        ESP_LOGI(TAG, "  [diag] USB modem diagnostic snapshot unavailable; netif=%p atparser=%p", (void *)ppp, (void *)at);
    }

    if (at == NULL) {
        if (verbose) {
            ESP_LOGI(TAG, "  [diag] AT parser handle is NULL: DTE not created/connected yet");
        }
        return false;
    }

    bool at_stopped = modem_at_is_stopped(at);
    if (at_stopped) {
        if (verbose) {
            ESP_LOGW(TAG, "  [diag] AT parser exists but is STOPPED: DTE is not connected or was disconnected");
        }
        return false;
    }
    set_boot_stage(MODEM_BOOT_STAGE_AT_PARSER_READY);

    esp_err_t err = at_cmd_at(at);
    if (err != ESP_OK) {
        if (verbose) {
            if (err == ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "  [diag] AT check failed: parser state invalid/stopped (%s)", esp_err_to_name(err));
            } else if (err == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "  [diag] AT check timeout: USB/DTE open but modem did not answer AT");
            } else if (err == ESP_ERR_NOT_FINISHED) {
                ESP_LOGW(TAG, "  [diag] AT check interrupted: parser stopped while waiting for response");
            } else {
                ESP_LOGW(TAG, "  [diag] AT check failed: %s", esp_err_to_name(err));
            }
        }
        return false;
    }
    set_boot_stage(MODEM_BOOT_STAGE_AT_OK);

    esp_modem_pin_state_t pin = PIN_UNKNOWN;
    err = at_cmd_read_pin(at, &pin);
    if (err != ESP_OK || pin != PIN_READY) {
        if (verbose) {
            ESP_LOGW(TAG, "  [diag] SIM not ready: err=%s state=%s(%d)", esp_err_to_name(err), pin_state_to_text(pin), pin);
        }
        return false;
    }
    set_boot_stage(MODEM_BOOT_STAGE_SIM_READY);

    esp_modem_at_csq_t csq = {0};
    err = at_cmd_get_signal_quality(at, &csq);
    if (err != ESP_OK || csq.rssi == 99 || csq.ber > 99) {
        if (verbose) {
            ESP_LOGW(TAG, "  [diag] signal not ready: err=%s rssi=%d ber=%d", esp_err_to_name(err), csq.rssi, csq.ber);
        }
        return false;
    }
    set_boot_stage(MODEM_BOOT_STAGE_SIGNAL_OK);

    esp_modem_at_cereg_t cereg = {0};
    err = at_cmd_get_network_reg_status(at, &cereg);
    if (err != ESP_OK || (cereg.stat != 1 && cereg.stat != 5)) {
        if (verbose) {
            ESP_LOGW(TAG, "  [diag] network not registered: err=%s n=%d stat=%d (%s)",
                     esp_err_to_name(err), cereg.n, cereg.stat, cereg_stat_to_text(cereg.stat));
        }
        return false;
    }
    set_boot_stage(MODEM_BOOT_STAGE_NETWORK_REGISTERED);

    char pdp[160] = {0};
    err = at_cmd_get_pdp_context(at, pdp, sizeof(pdp));
    if (verbose) {
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  [diag] PDP context: %s", pdp);
        } else {
            ESP_LOGW(TAG, "  [diag] read PDP context failed: %s", esp_err_to_name(err));
        }
    }
    set_boot_stage(MODEM_BOOT_STAGE_PPP_DIALING);
    return true;
}

static void log_final_failure_reason(bool kicked)
{
    ESP_LOGE(TAG, "  [diag] final stage: %s", boot_stage_to_text(s_boot_stage));
    ESP_LOGE(TAG, "  [diag] POWERKEY kick was %s", kicked ? "issued" : "not issued");
    ESP_LOGE(TAG, "  [diag] last PPP event: %s (%" PRId32 ")", ppp_event_to_text(s_last_ppp_event), s_last_ppp_event);

    switch (s_boot_stage) {
    case MODEM_BOOT_STAGE_USB_HOST_READY:
    case MODEM_BOOT_STAGE_USB_MODEM_INSTALLED:
        ESP_LOGE(TAG, "  [diag] likely cause: USB host ready but modem device never appeared; check power, cable, VID/PID, or USB wiring");
        break;
    case MODEM_BOOT_STAGE_USB_DEVICE_SEEN:
        ESP_LOGE(TAG, "  [diag] likely cause: USB device appeared but did not match modem ID or expected interfaces");
        break;
    case MODEM_BOOT_STAGE_USB_ID_MATCHED:
    case MODEM_BOOT_STAGE_USB_PORT_OPEN:
        ESP_LOGE(TAG, "  [diag] likely cause: matched device but CDC ports could not fully open or enumerate");
        break;
    case MODEM_BOOT_STAGE_DTE_CONNECTED:
        ESP_LOGE(TAG, "  [diag] likely cause: DTE connected but AT parser never started; check parser start/connect callback");
        break;
    case MODEM_BOOT_STAGE_AT_PARSER_READY:
        ESP_LOGE(TAG, "  [diag] likely cause: AT parser is running but modem did not answer AT; check USB data path, modem state, or serial interface mapping");
        break;
    case MODEM_BOOT_STAGE_AT_OK:
        ESP_LOGE(TAG, "  [diag] likely cause: SIM not inserted, SIM not ready, or SIM PIN/PUK required");
        break;
    case MODEM_BOOT_STAGE_SIM_READY:
        ESP_LOGE(TAG, "  [diag] likely cause: no antenna/signal or module has not reported usable signal yet");
        break;
    case MODEM_BOOT_STAGE_SIGNAL_OK:
        ESP_LOGE(TAG, "  [diag] likely cause: SIM not registered to network, no service, denied registration, or APN/operator issue");
        break;
    case MODEM_BOOT_STAGE_NETWORK_REGISTERED:
    case MODEM_BOOT_STAGE_PPP_DIALING:
        ESP_LOGE(TAG, "  [diag] likely cause: PPP dial/negotiation failed, APN/auth issue, or carrier rejected data call");
        break;
    default:
        ESP_LOGE(TAG, "  [diag] likely cause: modem did not reach PPP got-IP state before timeout");
        break;
    }
}


static void on_ppp_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    set_boot_stage(MODEM_BOOT_STAGE_PPP_GOT_IP);
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    if (event == NULL) {
        return;
    }

    ESP_LOGI(TAG, "  [event] 4G PPP got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "  [event] 4G gateway:    " IPSTR, IP2STR(&event->ip_info.gw));
    ESP_LOGI(TAG, "  [event] 4G mask:       " IPSTR, IP2STR(&event->ip_info.netmask));
    s_connected = event->ip_info.ip.addr != 0;
    s_dte_up = true;
}

static void on_ppp_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    ESP_LOGW(TAG, "  [event] 4G PPP lost IP");
    s_connected = false;
}

static void on_ppp_status(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    s_last_ppp_event = id;
    ESP_LOGI(TAG, "  [event] PPP status: %s (%" PRId32 ")", ppp_event_to_text(id), id);

    if (id == NETIF_PPP_PHASE_ESTABLISH || id == NETIF_PPP_PHASE_AUTHENTICATE || id == NETIF_PPP_PHASE_NETWORK) {
        set_boot_stage(MODEM_BOOT_STAGE_PPP_DIALING);
    } else if (id == NETIF_PPP_PHASE_RUNNING) {
        set_boot_stage(MODEM_BOOT_STAGE_PPP_GOT_IP);
    }
}

/*
 * Manual POWERKEY pulse when USB enumeration did not happen automatically.
 * AIR780E/AIR780ER PWRKEY is normally pulled high and is triggered by a
 * low-level pulse. If the board adds an inverter, flip POWERKEY_INACTIVE_LEVEL.
 */
static void powerkey_kick(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = BIT64(PIN_4G_PWRKEY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  POWERKEY gpio_config GPIO%d failed: %s",
                 PIN_4G_PWRKEY, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "  POWERKEY: GPIO%d  inactive_level=%d  active_level=%d",
             PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL, !POWERKEY_INACTIVE_LEVEL);
    ESP_LOGI(TAG, "  POWERKEY step 1/3: ensure inactive (level=%d)",
             POWERKEY_INACTIVE_LEVEL);
    gpio_set_level(PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL);
    ESP_LOGI(TAG, "  POWERKEY step 2/3: assert active (level=%d) for %d ms",
             !POWERKEY_INACTIVE_LEVEL, POWERKEY_ACTIVE_MS);
    gpio_set_level(PIN_4G_PWRKEY, !POWERKEY_INACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(POWERKEY_ACTIVE_MS));
    ESP_LOGI(TAG, "  POWERKEY step 3/3: release inactive (level=%d), settle %d ms",
             POWERKEY_INACTIVE_LEVEL, POWERKEY_SETTLE_MS);
    gpio_set_level(PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(POWERKEY_SETTLE_MS));
    ESP_LOGI(TAG, "  POWERKEY pulse done");
}

static void powerkey_idle(void)
{
    gpio_config_t io_config = {
        .pin_bit_mask = BIT64(PIN_4G_PWRKEY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  POWERKEY idle gpio_config GPIO%d failed: %s",
                 PIN_4G_PWRKEY, esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_set_level(PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL));
    ESP_LOGI(TAG, "  POWERKEY idle prepared: GPIO%d=%d", PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL);
}

static void modem_task(void *arg)
{
    (void)arg;

    s_connected = false;
    s_dte_up = false;
    s_boot_stage = MODEM_BOOT_STAGE_NOT_STARTED;
    s_last_ppp_event = -1;

    ESP_LOGI(TAG, "=== AIR780ER 4G modem init task start ===");

    ESP_LOGI(TAG, "[1/4] AIR780ER pins: PWRKEY=GPIO%d inactive=%d RESET=GPIO%d USB_DP=GPIO%d USB_DN=GPIO%d",
             PIN_4G_PWRKEY, POWERKEY_INACTIVE_LEVEL,
             PIN_4G_RESET, PIN_4G_USB_DP, PIN_4G_USB_DN);
    powerkey_idle();

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                                             &on_ppp_got_ip, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                                             &on_ppp_lost_ip, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                                             &on_ppp_status, NULL));

    ESP_LOGI(TAG, "[2/4] Installing USB CDC host driver");
    usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = USBH_CDC_TASK_STACK,
        .task_priority = USBH_CDC_TASK_PRIORITY,
        .task_coreid = CONFIG_USBH_TASK_CORE_ID,
        .skip_init_usb_host_driver = false,
    };
    esp_err_t err = usbh_cdc_driver_install(&cdc_config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "[2/4] USB CDC host driver already installed");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "[2/4] usbh_cdc_driver_install failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "=== AIR780ER modem init FAILED ===");
        s_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    } else {
        s_cdc_installed = true;
    }
    set_boot_stage(MODEM_BOOT_STAGE_USB_HOST_READY);

    ESP_LOGI(TAG, "[3/4] Installing iot_usbh_modem 2.x (VID=0x19D1 PID=0x0001 modem_itf=2 AT_itf=-1)");
    usbh_modem_config_t modem_config = {
        .modem_id_list = s_usb_modem_id_list,
        .at_tx_buffer_size = MODEM_AT_BUFFER_SIZE,
        .at_rx_buffer_size = MODEM_AT_BUFFER_SIZE,
    };
    err = usbh_modem_install(&modem_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[3/4] usbh_modem_install failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "=== AIR780ER modem init FAILED ===");
        s_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_modem_installed = true;
    set_boot_stage(MODEM_BOOT_STAGE_USB_MODEM_INSTALLED);
    ESP_LOGI(TAG, "[3/4] iot_usbh_modem installed OK");

    /*
     * iot_usbh_modem 2.x enables PPP auto-connect by default. It will detect
     * USB enumeration, run AT checks, and dial PPP from its modem_daemon task.
     */
    ESP_LOGI(TAG, "[4/4] Waiting for PPP: USB kick timeout=%d ms  boot timeout=%d ms",
             POWERKEY_KICK_DELAY_MS, BOOT_TIMEOUT_MS);

    bool kicked = false;
    TickType_t t0 = xTaskGetTickCount();
    uint32_t last_log_ms = 0;

    while (!s_connected) {
        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);

        if (elapsed_ms - last_log_ms >= WAIT_LOG_INTERVAL_MS) {
            esp_netif_t *ppp = usbh_modem_get_netif();
            esp_netif_ip_info_t ip = {0};
            bool has_ip = ppp != NULL &&
                          esp_netif_get_ip_info(ppp, &ip) == ESP_OK &&
                          ip.ip.addr != 0;
            if (has_ip) {
                ESP_LOGI(TAG, "[4/4] PPP netif has IP: " IPSTR, IP2STR(&ip.ip));
                s_connected = true;
                set_boot_stage(MODEM_BOOT_STAGE_PPP_GOT_IP);
                break;
            }

            poll_modem_diagnostics(true);
            at_handle_t at = usbh_modem_get_atparser();
            ESP_LOGI(TAG, "[4/4] %s: elapsed=%u ms connected=%d kicked=%d netif=%p at=%p stage=\"%s\"",
                     boot_wait_phase_to_text(s_boot_stage), elapsed_ms, s_connected, kicked,
                     (void *)ppp, (void *)at, boot_stage_to_text(s_boot_stage));
            last_log_ms = elapsed_ms;
        }

        if (!kicked && elapsed_ms >= POWERKEY_KICK_DELAY_MS && s_boot_stage < MODEM_BOOT_STAGE_USB_DEVICE_SEEN) {
            ESP_LOGW(TAG, "[4/4] USB/AT not ready after %u ms — issuing POWERKEY kick", elapsed_ms);
            powerkey_kick();
            kicked = true;
        }

        if (elapsed_ms >= BOOT_TIMEOUT_MS) {
            ESP_LOGE(TAG, "[4/4] Boot timeout after %u ms (limit=%d ms)", elapsed_ms, BOOT_TIMEOUT_MS);
            poll_modem_diagnostics(true);
            log_final_failure_reason(kicked);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (s_connected) {
        ESP_LOGI(TAG, "=== AIR780ER 4G modem ready — PPP up ===");
    } else {
        ESP_LOGE(TAG, "=== AIR780ER 4G modem FAILED to connect ===");
        log_final_failure_reason(kicked);
    }

    s_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t modem_4g_init(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Creating modem init task: name=modem_4g  stack=%d  prio=%d  affinity=ANY",
             MODEM_TASK_STACK, MODEM_TASK_PRIORITY);

    BaseType_t r = xTaskCreatePinnedToCore(modem_task, "modem_4g",
                                           MODEM_TASK_STACK, NULL,
                                           MODEM_TASK_PRIORITY,
                                           &s_task_handle, tskNO_AFFINITY);
    if (r != pdPASS) {
        s_task_handle = NULL;
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed (stack=%d) — out of memory?", MODEM_TASK_STACK);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "4G modem task created OK (PWRKEY=GPIO%d  RESET=GPIO%d  USB_DP=GPIO%d  USB_DN=GPIO%d)",
             PIN_4G_PWRKEY, PIN_4G_RESET, PIN_4G_USB_DP, PIN_4G_USB_DN);
    return ESP_OK;
}

bool modem_4g_is_connected(void)
{
    return s_connected;
}
