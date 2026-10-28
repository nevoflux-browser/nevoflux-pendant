#include "pendant_wifi.h"
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT  BIT0
#define RETRY_MIN_MS        1000
#define RETRY_MAX_MS        30000

static const char *TAG = "pendant_wifi";

static EventGroupHandle_t   s_events;
static esp_timer_handle_t   s_retry_timer;
static pendant_wifi_status_t s_status;
static uint32_t             s_retry_ms = RETRY_MIN_MS;
static bool                 s_inited;
static bool                 s_started;
static bool                 s_reconfiguring;

static void retry_cb(void *arg)
{
    (void)arg;
    s_status.state = PENDANT_WIFI_CONNECTING;
    (void)esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_status.state = PENDANT_WIFI_CONNECTING;
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        s_status.state = PENDANT_WIFI_FAILED;
        s_status.ip[0] = '\0';
        s_status.last_reason = ev ? ev->reason : 0;
        if (s_reconfiguring) {
            /* Our own disconnect before applying new credentials; pendant_wifi_start() reconnects. */
            s_reconfiguring = false;
            s_status.state = PENDANT_WIFI_CONNECTING;
            return;
        }
        ESP_LOGW(TAG, "disconnected from \"%s\" (reason %u), retry in %lums",
                 s_status.ssid, (unsigned)s_status.last_reason, (unsigned long)s_retry_ms);
        (void)esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000ULL);
        s_retry_ms = (s_retry_ms * 2 > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_status.rssi = ap.rssi;
        }
        s_status.state = PENDANT_WIFI_CONNECTED;
        s_retry_ms = RETRY_MIN_MS;
        ESP_LOGI(TAG, "connected to \"%s\", ip=%s rssi=%d", s_status.ssid, s_status.ip, s_status.rssi);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_stack_init(void)
{
    if (s_inited) {
        return true;
    }
    s_events = xEventGroupCreate();
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    (void)esp_netif_set_hostname(netif, "nevoflux-pendant");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM); /* credentials live in our own `pendant` namespace */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_inited = true;
    return true;
}

bool pendant_wifi_start(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        ssid = CONFIG_PENDANT_WIFI_DEFAULT_SSID;
        password = CONFIG_PENDANT_WIFI_DEFAULT_PASSWORD;
    }
    if (!password) {
        password = "";
    }
    if (ssid[0] == '\0') {
        s_status.state = PENDANT_WIFI_OFF;
        ESP_LOGW(TAG, "no SSID configured; provision with `provision.py --wifi-only`");
        return false;
    }
    if (!wifi_stack_init()) {
        ESP_LOGE(TAG, "WiFi stack init failed");
        return false;
    }

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", password);
    /* Accept open networks only when no password was given. */
    wc.sta.threshold.authmode = (password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", ssid);
    s_status.ip[0] = '\0';
    s_status.state = PENDANT_WIFI_CONNECTING;
    s_retry_ms = RETRY_MIN_MS;

    (void)esp_timer_stop(s_retry_timer);
    if (s_started) {
        s_reconfiguring = (esp_wifi_disconnect() == ESP_OK);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        (void)esp_wifi_connect();
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_ERROR_CHECK(esp_wifi_start()); /* WIFI_EVENT_STA_START triggers the first connect */
        /* Modem sleep (the default) leaves downlink parked at the AP between
         * beacons; phone hotspots buffer it badly, so a reply arriving mid-TLS
         * record stalled past the socket's 10 s read timeout and the WebSocket
         * dropped. Stay awake: the radio costs some battery, the link stays up. */
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
        s_started = true;
    }
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return true;
}

void pendant_wifi_get_status(pendant_wifi_status_t *out)
{
    if (!out) {
        return;
    }
    if (s_status.state == PENDANT_WIFI_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_status.rssi = ap.rssi;
        }
    }
    *out = s_status;
}

bool pendant_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_events) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void pendant_wifi_format_line(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    pendant_wifi_status_t st;
    pendant_wifi_get_status(&st);
    switch (st.state) {
    case PENDANT_WIFI_OFF:
        snprintf(out, cap, "WiFi 未配置");
        break;
    case PENDANT_WIFI_CONNECTING:
        snprintf(out, cap, "WiFi 连接中: %s", st.ssid);
        break;
    case PENDANT_WIFI_CONNECTED:
        snprintf(out, cap, "%s · %s · %ddBm", st.ssid, st.ip, st.rssi);
        break;
    case PENDANT_WIFI_FAILED:
    default:
        snprintf(out, cap, "WiFi 连接失败(%u): %s", (unsigned)st.last_reason, st.ssid);
        break;
    }
}
