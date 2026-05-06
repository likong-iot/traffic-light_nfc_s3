#include "peripheral_test.h"

#include <inttypes.h>
#include <netdb.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"
#include "storage_sd.h"

static const char *TAG = "periph_test";

#define TEST_TASK_STACK          6144
#define TEST_TASK_PRIORITY       2
#define TEST_WAIT_NETWORK_MS     120000
#define TEST_WAIT_POLL_MS        1000
#define TEST_PING_TARGET_HOST    "www.baidu.com"
#define TEST_PING_COUNT          4
#define TEST_PING_INTERVAL_MS    1000
#define TEST_PING_TIMEOUT_MS     3000
#define TEST_PING_DATA_SIZE      32

static TaskHandle_t s_test_task = NULL;

typedef struct {
    const char *name;
    uint32_t replies;
    uint32_t timeouts;
    SemaphoreHandle_t done;
} ping_context_t;

static bool netif_has_ipv4(esp_netif_t *netif, esp_netif_ip_info_t *ip_info)
{
    if (netif == NULL || ip_info == NULL) {
        return false;
    }
    if (esp_netif_get_ip_info(netif, ip_info) != ESP_OK) {
        return false;
    }
    return ip_info->ip.addr != 0;
}

static esp_netif_t *wait_netif_ip(const char *if_key, const char *name, uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (waited_ms <= timeout_ms) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_key);
        esp_netif_ip_info_t ip = {0};
        if (netif_has_ipv4(netif, &ip)) {
            ESP_LOGI(TAG, "[NET] %s ready: ip=" IPSTR " mask=" IPSTR " gw=" IPSTR,
                     name, IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw));
            return netif;
        }

        if ((waited_ms % 5000) == 0) {
            ESP_LOGI(TAG, "[NET] waiting for %s IPv4 (%" PRIu32 "/%" PRIu32 " ms)",
                     name, waited_ms, timeout_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(TEST_WAIT_POLL_MS));
        waited_ms += TEST_WAIT_POLL_MS;
    }

    ESP_LOGW(TAG, "[NET] %s IPv4 wait timed out", name);
    return NULL;
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_context_t *ctx = (ping_context_t *)args;
    uint8_t ttl = 0;
    uint16_t seqno = 0;
    uint32_t elapsed_time = 0;
    uint32_t recv_len = 0;
    ip_addr_t target_addr = {0};

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));

    if (ctx != NULL) {
        ctx->replies++;
    }
    ESP_LOGI(TAG, "[PING:%s] %" PRIu32 " bytes from %s icmp_seq=%u ttl=%u time=%" PRIu32 " ms",
             ctx ? ctx->name : "-", recv_len, ipaddr_ntoa(&target_addr), seqno, ttl, elapsed_time);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    ping_context_t *ctx = (ping_context_t *)args;
    uint16_t seqno = 0;
    ip_addr_t target_addr = {0};

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));

    if (ctx != NULL) {
        ctx->timeouts++;
    }
    ESP_LOGW(TAG, "[PING:%s] from %s icmp_seq=%u timeout",
             ctx ? ctx->name : "-", ipaddr_ntoa(&target_addr), seqno);
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    ping_context_t *ctx = (ping_context_t *)args;
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &duration, sizeof(duration));

    ESP_LOGI(TAG, "[PING:%s] end: tx=%" PRIu32 " rx=%" PRIu32 " duration=%" PRIu32 " ms",
             ctx ? ctx->name : "-", transmitted, received, duration);
    if (ctx != NULL && ctx->done != NULL) {
        xSemaphoreGive(ctx->done);
    }
}

static esp_err_t resolve_ping_target(ip_addr_t *target_addr)
{
    if (target_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_RAW,
    };
    struct addrinfo *res = NULL;

    int rc = getaddrinfo(TEST_PING_TARGET_HOST, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        ESP_LOGE(TAG, "[PING] resolve %s failed: %d", TEST_PING_TARGET_HOST, rc);
        return ESP_FAIL;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_addr_to_ip4addr(ip_2_ip4(target_addr), &addr->sin_addr);
    IP_SET_TYPE(target_addr, IPADDR_TYPE_V4);
    ESP_LOGI(TAG, "[PING] resolved %s -> %s", TEST_PING_TARGET_HOST, ipaddr_ntoa(target_addr));
    freeaddrinfo(res);
    return ESP_OK;
}

static esp_err_t ping_from_netif(const char *name, esp_netif_t *netif, const ip_addr_t *target_addr)
{
    if (name == NULL || netif == NULL || target_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int if_index = esp_netif_get_netif_impl_index(netif);
    if (if_index <= 0) {
        ESP_LOGE(TAG, "[PING:%s] invalid netif index=%d", name, if_index);
        return ESP_FAIL;
    }

    ping_context_t ctx = {
        .name = name,
        .done = xSemaphoreCreateBinary(),
    };
    if (ctx.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = *target_addr;
    config.count = TEST_PING_COUNT;
    config.interval_ms = TEST_PING_INTERVAL_MS;
    config.timeout_ms = TEST_PING_TIMEOUT_MS;
    config.data_size = TEST_PING_DATA_SIZE;
    config.interface = (uint32_t)if_index;

    esp_ping_callbacks_t cbs = {
        .cb_args = &ctx,
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = on_ping_end,
    };

    ESP_LOGI(TAG, "[PING:%s] start %s via if_index=%d count=%d",
             name, ipaddr_ntoa(target_addr), if_index, TEST_PING_COUNT);

    esp_ping_handle_t ping = NULL;
    esp_err_t err = esp_ping_new_session(&config, &cbs, &ping);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[PING:%s] create session failed: %s", name, esp_err_to_name(err));
        vSemaphoreDelete(ctx.done);
        return err;
    }

    err = esp_ping_start(ping);
    if (err == ESP_OK) {
        uint32_t wait_ms = TEST_PING_COUNT * (TEST_PING_INTERVAL_MS + TEST_PING_TIMEOUT_MS) + 3000;
        if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
            ESP_LOGW(TAG, "[PING:%s] wait end timeout", name);
            err = ESP_ERR_TIMEOUT;
        }
    } else {
        ESP_LOGE(TAG, "[PING:%s] start failed: %s", name, esp_err_to_name(err));
    }

    esp_ping_delete_session(ping);
    vSemaphoreDelete(ctx.done);

    if (err == ESP_OK && ctx.replies == 0) {
        ESP_LOGW(TAG, "[PING:%s] no replies received", name);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[PING:%s] result: replies=%" PRIu32 " timeouts=%" PRIu32,
             name, ctx.replies, ctx.timeouts);
    return err;
}

static void peripheral_test_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "=== peripheral test task started ===");

    if (storage_sd_is_mounted()) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(storage_sd_self_test());
    } else {
        ESP_LOGW(TAG, "[SD] self-test skipped: storage is not mounted");
    }

    esp_netif_t *eth = wait_netif_ip("ETH_DEF", "W5500/ETH", TEST_WAIT_NETWORK_MS);
    esp_netif_t *ppp = wait_netif_ip("PPP_DEF", "AIR780ER/PPP", TEST_WAIT_NETWORK_MS);

    ip_addr_t target_addr = {0};
    if (resolve_ping_target(&target_addr) == ESP_OK) {
        if (eth != NULL) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(ping_from_netif("ETH", eth, &target_addr));
        }
        if (ppp != NULL) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(ping_from_netif("PPP", ppp, &target_addr));
        }
    }

    ESP_LOGI(TAG, "=== peripheral test task finished ===");
    s_test_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t peripheral_test_start(void)
{
    if (s_test_task != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting peripheral test task: stack=%d prio=%d",
             TEST_TASK_STACK, TEST_TASK_PRIORITY);
    BaseType_t ok = xTaskCreatePinnedToCore(peripheral_test_task,
                                            "periph_test",
                                            TEST_TASK_STACK,
                                            NULL,
                                            TEST_TASK_PRIORITY,
                                            &s_test_task,
                                            tskNO_AFFINITY);
    if (ok != pdPASS) {
        s_test_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
