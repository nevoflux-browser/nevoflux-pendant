#include "nf_link.h"
#include "nf_addr.h"
#include "nf_resolve.h"
#include "nevoflux_app.h"
#include "pendant_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mdns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "nf_link";

#define NF_ACCOUNT_HOST        "nevoflux.app"
#define NF_ACCOUNT_TOKEN_URL   "https://" NF_ACCOUNT_HOST "/api/auth/token"
#define NF_MAX_MESSAGE_BYTES   (2U * 1024U * 1024U) /* a long spoken sentence is ~0.6 MB */
/* Pointers to PSRAM copies; deep enough for a reply's burst of deltas */
#define NF_INBOX_DEPTH         96
/* 32 x 200 ms speech chunks = 6.4 s of uplink the network may fall behind by */
#define NF_OUTBOX_DEPTH        32
#define NF_LAN_PROBE_MS        1500
#define NF_LAN_REPROBE_MS      (2U * 60U * 1000U)
/*
 * A healthy socket hears at least the relay's "pong" every 30 s. Paths to the
 * relay go half-dead (sends still accepted, nothing comes back) before they are
 * reset, and a reply sent into one is lost: notice within 1.5 intervals.
 */
#define NF_SILENT_DEADLINE_MS  (45U * 1000U)
#define NF_BACKOFF_MIN_MS      1000U
#define NF_BACKOFF_MAX_MS      60000U

typedef struct {
    esp_websocket_client_handle_t client;
    bool          is_data;
    uint8_t       key[NF_AES_KEY_SIZE];
    volatile bool connected;
    volatile int  peers;          /* -1 = not told yet */
    volatile uint64_t last_rx_ms;
    uint64_t      up_ms;          /* when it connected; for the drop log */
    uint32_t      rx_bytes;       /* received since then */
    /* reassembly of one message across DATA events */
    uint8_t      *rx_buf;
    size_t        rx_len;
    size_t        rx_cap;
    bool          rx_binary;
} nf_chan_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_inbox;
static QueueHandle_t     s_outbox;  /* data frames to send, PSRAM copies */
static SemaphoreHandle_t s_tx_lock; /* a send never races the data client's teardown */
static nf_credentials_t  s_cred;
static volatile bool     s_restart;
static nf_chan_t         s_ctl = {.is_data = false, .peers = -1};
static nf_chan_t         s_data = {.is_data = true, .peers = -1};
static nf_link_status_t  s_status;
static char              s_attached_session[96];
static char              s_pending_mode[8];   /* set_mode for the link task to send */

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

/* Milliseconds from `then` to `t`; 0 when another task stamped `then` after `t` was read */
static uint64_t ms_since(uint64_t t, uint64_t then)
{
    return t > then ? t - then : 0U;
}

static void *ps_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* ------------------------------------------------------------------ sealing */

static bool chan_send_plain(nf_chan_t *ch, const char *plaintext)
{
    if (!ch->client || !ch->connected) {
        return false;
    }
    size_t pt_len = strlen(plaintext);
    size_t cap = pt_len + NF_AES_OVERHEAD;
    uint8_t *wire = ps_malloc(cap);
    if (!wire) {
        return false;
    }
    uint8_t nonce[NF_AES_NONCE_SIZE];
    esp_fill_random(nonce, sizeof(nonce));
    size_t n = nf_channel_encrypt(ch->key, nonce, (const uint8_t *)plaintext, pt_len, wire, cap);
    bool ok = n > 0U && esp_websocket_client_send_bin(ch->client, (const char *)wire, (int)n,
                                                      pdMS_TO_TICKS(3000)) == (int)n;
    free(wire);
    return ok;
}

static bool chan_send_frame(nf_chan_t *ch, const char *frame_json)
{
    size_t cap = strlen(frame_json) + 32U;
    char *wire = ps_malloc(cap);
    if (!wire) {
        return false;
    }
    bool ok = nf_wire_wrap_frame(frame_json, wire, cap) > 0 && chan_send_plain(ch, wire);
    free(wire);
    return ok;
}

/* ------------------------------------------------------------------ control channel */

/* The most recently updated conversation on the list: what the pendant talks into. */
static void on_sessions(cJSON *frame)
{
    cJSON *rows = cJSON_GetObjectItem(frame, "rows");
    const cJSON *best = NULL;
    double best_t = -1.0;
    const char *best_s = NULL;
    cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        cJSON *u = cJSON_GetObjectItem(row, "updatedAt");
        if (cJSON_IsNumber(u)) {
            if (u->valuedouble > best_t) {
                best_t = u->valuedouble;
                best = row;
            }
        } else if (cJSON_IsString(u)) {
            /* ISO-8601 sorts as text */
            if (!best_s || strcmp(u->valuestring, best_s) > 0) {
                best_s = u->valuestring;
                if (best_t < 0.0) {
                    best = row;
                }
            }
        } else if (!best) {
            best = row;
        }
    }
    cJSON *id = best ? cJSON_GetObjectItem(best, "id") : NULL;
    if (!cJSON_IsString(id) || !id->valuestring[0]) {
        ESP_LOGW(TAG, "the head has no conversation to attach to yet");
        return;
    }
    cJSON *title = cJSON_GetObjectItem(best, "title");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.session_title, sizeof(s_status.session_title), "%s",
             cJSON_IsString(title) ? title->valuestring : "");
    xSemaphoreGive(s_lock);
    if (strcmp(s_attached_session, id->valuestring) == 0 && s_status.attached) {
        return; /* already showing it; a fresh list is not a reason to rebind */
    }
    char json[160];
    snprintf(json, sizeof(json), "{\"kind\":\"attach\",\"sessionId\":\"%.100s\"}", id->valuestring);
    if (chan_send_frame(&s_ctl, json)) {
        snprintf(s_attached_session, sizeof(s_attached_session), "%s", id->valuestring);
        s_status.attached = true;
        ESP_LOGI(TAG, "attached to session %s", id->valuestring);
    }
}

static void on_control_plain(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return;
    }
    cJSON *frame = cJSON_GetObjectItem(root, "frame");
    cJSON *kind = frame ? cJSON_GetObjectItem(frame, "kind") : NULL;
    if (cJSON_IsString(kind) && strcmp(kind->valuestring, "sessions") == 0) {
        on_sessions(frame);
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ socket events */

/* A complete message off one socket. */
static void on_message(nf_chan_t *ch, uint8_t *buf, size_t len, bool binary)
{
    if (!binary) {
        /* Plaintext only ever comes from the relay (or the LAN listener) itself */
        int peers = 0;
        nf_relay_text_kind_t k = nf_relay_classify_text((const char *)buf, len, &peers);
        if (k == NF_RELAY_TEXT_PEERS) {
            ch->peers = peers;
        }
        return;
    }
    if (len < NF_AES_OVERHEAD) {
        return;
    }
    size_t pt_cap = len - NF_AES_OVERHEAD + 1U;
    uint8_t *pt = ps_malloc(pt_cap);
    if (!pt) {
        return;
    }
    size_t pt_len = 0U;
    if (!nf_channel_decrypt(ch->key, buf, len, pt, pt_cap, &pt_len)) {
        ESP_LOGW(TAG, "a sealed frame would not open: is the pendant paired with this head?");
        free(pt);
        return;
    }
    pt[pt_len] = '\0';
    if (!ch->is_data) {
        on_control_plain((const char *)pt);
        free(pt);
        return;
    }
    if (xQueueSend(s_inbox, &pt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "inbox full; dropping a frame");
        free(pt);
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)base;
    nf_chan_t *ch = (nf_chan_t *)arg;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ch->connected = true;
        ch->last_rx_ms = now_ms();
        ch->up_ms = ch->last_rx_ms;
        ch->rx_bytes = 0U;
        ESP_LOGI(TAG, "%s socket connected", ch->is_data ? "data" : "control");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        if (ch->connected) {
            uint64_t t = now_ms();
            ESP_LOGW(TAG, "%s socket %s after %us up, %ums since data, %u B in",
                     ch->is_data ? "data" : "control",
                     id == WEBSOCKET_EVENT_CLOSED ? "closed" : (id == WEBSOCKET_EVENT_ERROR ? "error" : "dropped"),
                     (unsigned)((t - ch->up_ms) / 1000ULL), (unsigned)ms_since(t, ch->last_rx_ms),
                     (unsigned)ch->rx_bytes);
        }
        ch->connected = false;
        break;
    case WEBSOCKET_EVENT_DATA: {
        ch->last_rx_ms = now_ms();
        ch->rx_bytes += (uint32_t)d->data_len;
        if (d->op_code >= 0x08) {
            break; /* close / ping / pong: the client answers those itself */
        }
        if (d->op_code == 0x01 || d->op_code == 0x02) {
            if (d->payload_offset == 0) {
                ch->rx_len = 0U;
                ch->rx_binary = (d->op_code == 0x02);
            }
        }
        size_t total = (size_t)d->payload_offset + (size_t)d->data_len;
        size_t need = ch->rx_len + (size_t)d->data_len + 1U;
        if ((size_t)d->payload_len + 1U > need && d->payload_offset == 0) {
            need = (size_t)d->payload_len + 1U;
        }
        if (need > NF_MAX_MESSAGE_BYTES) {
            ESP_LOGW(TAG, "message too large (%u B); dropped", (unsigned)need);
            ch->rx_len = 0U;
            break;
        }
        if (need > ch->rx_cap) {
            uint8_t *nb = heap_caps_realloc(ch->rx_buf, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) {
                ch->rx_len = 0U;
                break;
            }
            ch->rx_buf = nb;
            ch->rx_cap = need;
        }
        if (d->data_len > 0) {
            memcpy(ch->rx_buf + ch->rx_len, d->data_ptr, (size_t)d->data_len);
            ch->rx_len += (size_t)d->data_len;
        }
        if (d->fin && total >= (size_t)d->payload_len) {
            ch->rx_buf[ch->rx_len] = 0U;
            on_message(ch, ch->rx_buf, ch->rx_len, ch->rx_binary);
            ch->rx_len = 0U;
        }
        break;
    }
    default:
        break;
    }
}

static void chan_close(nf_chan_t *ch)
{
    if (ch->client) {
        (void)esp_websocket_client_close(ch->client, pdMS_TO_TICKS(1000));
        (void)esp_websocket_client_destroy(ch->client);
        ch->client = NULL;
    }
    ch->connected = false;
    ch->peers = -1;
    ch->rx_len = 0U;
}

static bool chan_open(nf_chan_t *ch, const char *url, bool tls)
{
    esp_websocket_client_config_t cfg = {
        .uri = url,
        .buffer_size = 8192,
        .task_stack = 12288, /* TLS handshake with certificate-bundle verification */
        .task_prio = 5,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 30,
        .pingpong_timeout_sec = 45,
        .user_agent = NF_HTTP_USER_AGENT,
    };
    if (tls) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    ch->client = esp_websocket_client_init(&cfg);
    if (!ch->client) {
        return false;
    }
    esp_websocket_register_events(ch->client, WEBSOCKET_EVENT_ANY, ws_event, ch);
    ch->peers = -1;
    ch->connected = false;
    if (esp_websocket_client_start(ch->client) != ESP_OK) {
        chan_close(ch);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ discovery & token */

typedef struct {
    char     ip[16];
    uint16_t port;
} nf_lan_head_t;

static bool lan_find_head(nf_lan_head_t *out)
{
    char tag[9];
    nf_lan_channel_tag(s_cred.data_channel_id, tag);
    mdns_result_t *results = NULL;
    if (mdns_query_ptr("_nevoflux", "_tcp", NF_LAN_PROBE_MS, 8, &results) != ESP_OK || !results) {
        return false;
    }
    bool found = false;
    for (mdns_result_t *r = results; r && !found; r = r->next) {
        bool ours = false;
        for (size_t i = 0; i < r->txt_count; ++i) {
            if (r->txt[i].key && strcmp(r->txt[i].key, "ch") == 0 && r->txt[i].value &&
                nf_lan_txt_has_tag(r->txt[i].value, tag)) {
                ours = true;
            }
        }
        if (!ours) {
            continue;
        }
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                out->port = r->port;
                found = true;
                break;
            }
        }
    }
    mdns_query_results_free(results);
    return found;
}

typedef struct {
    char  *body;
    size_t len;
    size_t cap;
    char   header_jwt[1024];
} http_acc_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_acc_t *acc = (http_acc_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "set-auth-jwt") == 0) {
        snprintf(acc->header_jwt, sizeof(acc->header_jwt), "%s", evt->header_value);
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (acc->len + (size_t)evt->data_len + 1U <= acc->cap) {
            memcpy(acc->body + acc->len, evt->data, (size_t)evt->data_len);
            acc->len += (size_t)evt->data_len;
            acc->body[acc->len] = '\0';
        }
    }
    return ESP_OK;
}

/*
 * After a failure to reach the relay: the same TCP connect, from this pendant at
 * this moment, to Cloudflare and to hosts that are not on it. Only Cloudflare
 * failing points at the path there; everything failing points at this side (the
 * hotspot, Wi-Fi, or this firmware). Plain TCP, no TLS; at most once a minute.
 */
static int probe_connect_ms(const char *host, const char *port, char ip_out[16])
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    snprintf(ip_out, 16, "?");
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        return -2; /* DNS */
    }
    struct sockaddr_in sa = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    inet_ntoa_r(sa.sin_addr, ip_out, 16);
    return nf_resolve_tcp_ms(sa.sin_addr.s_addr, (uint16_t)atoi(port), 4000);
}

static void probe_network(const char *why)
{
    static uint64_t last;
    uint64_t t = now_ms();
    if (last && t - last < 60000ULL) {
        return;
    }
    last = t;
    /* Both Cloudflare addresses serve both names; DNS rotates their order. */
    static const char *const hosts[][2] = {
        {"relay.nevoflux.app", "443"}, {"104.21.63.34", "443"}, {"104.21.63.34", "80"},
        {"172.67.169.111", "443"},     {"172.67.169.111", "80"}, /* Cloudflare */
        {"www.baidu.com", "443"},      {"223.5.5.5", "443"},     /* not Cloudflare */
    };
    char line[320];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); ++i) {
        char ip[16];
        int ms = probe_connect_ms(hosts[i][0], hosts[i][1], ip);
        char what[16];
        if (ms >= 0) {
            snprintf(what, sizeof(what), "%dms", ms);
        } else {
            snprintf(what, sizeof(what), "%s", ms == -1 ? "TIMEOUT" : ms == -2 ? "DNS-FAIL" : ms == -3 ? "NO-SOCKET" : "REFUSED");
        }
        n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s:%s(%s)=%s", i ? " " : "", hosts[i][0], hosts[i][1], ip, what);
        if (n >= sizeof(line)) {
            break;
        }
    }
    ESP_LOGW(TAG, "net probe (%s): %s", why, line);
}

/* A fresh 15-minute relay JWT. The relay only checks it at the upgrade. */
static bool fetch_jwt(nf_token_mgr_t *tm)
{
    static http_acc_t acc; /* only the link task fetches; keeps 1 KB off the TLS stack */
    memset(&acc, 0, sizeof(acc));
    acc.cap = 4096;
    acc.body = ps_malloc(acc.cap);
    if (!acc.body) {
        return false;
    }
    acc.body[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = NF_ACCOUNT_TOKEN_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &acc,
        .user_agent = NF_HTTP_USER_AGENT, /* Cloudflare 1010 refuses an empty one */
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    bool ok = false;
    if (h) {
        char auth[300];
        snprintf(auth, sizeof(auth), "Bearer %s", s_cred.account_token);
        esp_http_client_set_header(h, "Authorization", auth);
        esp_http_client_set_header(h, "Accept", "application/json");
        if (esp_http_client_perform(h) == ESP_OK) {
            int status = esp_http_client_get_status_code(h);
            if (status == 200 && acc.header_jwt[0]) {
                ok = nf_token_mgr_accept_header_jwt(tm, acc.header_jwt, now_ms());
            } else {
                ok = nf_token_mgr_parse_response(tm, status, acc.body, now_ms());
            }
            if (!ok) {
                ESP_LOGW(TAG, "relay token: HTTP %d", status);
            }
        }
        esp_http_client_cleanup(h);
    }
    free(acc.body);
    return ok;
}

/* ------------------------------------------------------------------ the task */

static void set_mode(nf_link_mode_t m)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.mode = m;
    if (m == NF_LINK_DOWN) {
        s_status.data_up = false;
        s_status.head_online = false;
        s_status.attached = false;
    }
    xSemaphoreGive(s_lock);
}

static void link_task(void *arg)
{
    (void)arg;
    /* Static, not on the stack: the TLS handshake below needs that room */
    static nf_token_mgr_t tm;
    nf_token_mgr_init(&tm);
    uint32_t backoff = NF_BACKOFF_MIN_MS;
    bool mdns_ready = false;

    for (;;) {
        set_mode(NF_LINK_DOWN);
        s_attached_session[0] = '\0';
        s_restart = false;
        if (!s_cred.is_paired) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!pendant_wifi_wait_connected(5000)) {
            continue;
        }
        ESP_LOGI(TAG, "heap: internal free %u (largest %u), psram free %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        static bool booted_probe;
        if (!booted_probe) {
            booted_probe = true;
            probe_network("boot, before any connection"); /* no earlier socket can be in the way */
        }
        if (!mdns_ready && mdns_init() == ESP_OK) {
            (void)mdns_hostname_set("nevoflux-pendant");
            mdns_ready = true;
        }

        nf_lan_head_t head = {0};
        bool lan = mdns_ready && lan_find_head(&head);
        static char ctl_url[1400];
        static char data_url[1400];
        static char relay_host[64];
        if (lan) {
            ESP_LOGI(TAG, "head found on the LAN at %s:%u", head.ip, (unsigned)head.port);
            (void)nf_lan_build_ws_url(head.ip, head.port, s_cred.control_channel_id, ctl_url, sizeof(ctl_url));
            (void)nf_lan_build_ws_url(head.ip, head.port, s_cred.data_channel_id, data_url, sizeof(data_url));
        } else {
            if (tm.auth_revoked) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.auth_revoked = true;
                xSemaphoreGive(s_lock);
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
            nf_resolve_pick(NF_ACCOUNT_HOST);
            if (!fetch_jwt(&tm)) {
                nf_resolve_failed(NF_ACCOUNT_HOST);
                probe_network("token failed");
                vTaskDelay(pdMS_TO_TICKS(backoff));
                backoff = backoff * 2U > NF_BACKOFF_MAX_MS ? NF_BACKOFF_MAX_MS : backoff * 2U;
                continue;
            }
            const char *relay = s_cred.relay_url[0] ? s_cred.relay_url : "wss://relay.nevoflux.app";
            if (nf_addr_url_host(relay, relay_host, sizeof(relay_host))) {
                nf_resolve_pick(relay_host);
            } else {
                relay_host[0] = '\0';
            }
            (void)nf_relay_build_ws_url(relay, s_cred.control_channel_id, false, tm.jwt, ctl_url, sizeof(ctl_url));
            (void)nf_relay_build_ws_url(relay, s_cred.data_channel_id, false, tm.jwt, data_url, sizeof(data_url));
        }

        memcpy(s_ctl.key, s_cred.control_key, NF_AES_KEY_SIZE);
        memcpy(s_data.key, s_cred.data_key, NF_AES_KEY_SIZE);
        bool opened = chan_open(&s_ctl, ctl_url, !lan) && chan_open(&s_data, data_url, !lan);
        uint64_t t0 = now_ms();
        while (opened && (!s_ctl.connected || !s_data.connected) && now_ms() - t0 < 30000ULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!opened || !s_ctl.connected || !s_data.connected) {
            ESP_LOGW(TAG, "could not reach the head via %s", lan ? "LAN" : "relay");
            if (!lan) {
                nf_resolve_failed(relay_host);
                probe_network("relay failed");
            }
            chan_close(&s_ctl);
            chan_close(&s_data);
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = backoff * 2U > NF_BACKOFF_MAX_MS ? NF_BACKOFF_MAX_MS : backoff * 2U;
            continue;
        }

        set_mode(lan ? NF_LINK_LAN : NF_LINK_RELAY);
        (void)chan_send_frame(&s_ctl, "{\"kind\":\"sync\"}");
        uint64_t up_at = now_ms();
        uint64_t last_ping = up_at;
        uint64_t last_probe = up_at;
        bool switch_to_lan = false;

        while (s_ctl.connected && s_data.connected && !s_restart && !switch_to_lan) {
            vTaskDelay(pdMS_TO_TICKS(250));
            uint64_t t = now_ms();
            if (t - last_ping >= NF_WS_KEEPALIVE_INTERVAL_MS) {
                /* What the relay watches; answered below its Durable Object, so free */
                (void)esp_websocket_client_send_text(s_ctl.client, "ping", 4, pdMS_TO_TICKS(1000));
                xSemaphoreTake(s_tx_lock, portMAX_DELAY);
                (void)esp_websocket_client_send_text(s_data.client, "ping", 4, pdMS_TO_TICKS(1000));
                xSemaphoreGive(s_tx_lock);
                last_ping = t;
            }
            if (ms_since(t, s_data.last_rx_ms) > NF_SILENT_DEADLINE_MS ||
                ms_since(t, s_ctl.last_rx_ms) > NF_SILENT_DEADLINE_MS) {
                ESP_LOGW(TAG, "socket went silent; reconnecting");
                if (!lan) {
                    nf_resolve_failed(relay_host); /* a black-holed path: try the other address first */
                }
                break;
            }
            /* On the relay the head is there when the relay says so; it pushes a
             * peers notice when it arrives, so an empty channel is just held open
             * (an idle socket costs nothing) rather than redialled. */
            char mode[sizeof(s_pending_mode)];
            xSemaphoreTake(s_lock, portMAX_DELAY);
            snprintf(mode, sizeof(mode), "%s", s_pending_mode);
            s_pending_mode[0] = '\0';
            xSemaphoreGive(s_lock);
            if (mode[0] && s_attached_session[0]) {
                char json[192];
                snprintf(json, sizeof(json), "{\"kind\":\"set_mode\",\"sessionId\":\"%.100s\",\"mode\":\"%s\"}",
                         s_attached_session, mode);
                ESP_LOGI(TAG, "asking the head for %s mode", mode);
                if (!chan_send_frame(&s_ctl, json)) {
                    ESP_LOGW(TAG, "set_mode not sent");
                }
            }
            bool online = lan ? true : (s_data.peers > 0);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.data_up = true;
            s_status.head_online = online;
            xSemaphoreGive(s_lock);
            if (!lan && mdns_ready && t - last_probe >= NF_LAN_REPROBE_MS) {
                last_probe = t;
                nf_lan_head_t h2;
                switch_to_lan = lan_find_head(&h2);
                if (switch_to_lan) {
                    ESP_LOGI(TAG, "head appeared on the LAN; moving off the relay");
                }
            }
        }

        bool was_stable = now_ms() - up_at >= 5000ULL;
        chan_close(&s_ctl);
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        chan_close(&s_data);
        xSemaphoreGive(s_tx_lock);
        set_mode(NF_LINK_DOWN);
        if (switch_to_lan || s_restart) {
            backoff = NF_BACKOFF_MIN_MS;
            continue;
        }
        if (was_stable) {
            /* A reset of a working socket (the relay path does that every few
             * minutes on some networks): redial at once so it goes unnoticed */
            ESP_LOGI(TAG, "link lost after %us; redialling", (unsigned)((now_ms() - up_at) / 1000ULL));
            backoff = NF_BACKOFF_MIN_MS;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = backoff * 2U > NF_BACKOFF_MAX_MS ? NF_BACKOFF_MAX_MS : backoff * 2U;
    }
}

/*
 * Data frames go out from here, not from the caller: a TLS send to the relay can
 * take longer than the I2S DMA holds (a mic task blocked in it loses audio), and
 * the client lock is held for as long as a large voice_audio frame takes to arrive.
 */
static void sender_task(void *arg)
{
    (void)arg;
    for (;;) {
        char *frame = NULL;
        if (xQueueReceive(s_outbox, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        if (s_data.connected && !chan_send_frame(&s_data, frame)) {
            ESP_LOGW(TAG, "data frame not sent");
        }
        xSemaphoreGive(s_tx_lock);
        free(frame);
    }
}

/* ------------------------------------------------------------------ API */

bool nf_link_start(const nf_credentials_t *cred)
{
    if (s_lock) {
        nf_link_restart(cred);
        return true;
    }
    s_lock = xSemaphoreCreateMutex();
    s_inbox = xQueueCreate(NF_INBOX_DEPTH, sizeof(char *));
    s_outbox = xQueueCreate(NF_OUTBOX_DEPTH, sizeof(char *));
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_inbox || !s_outbox || !s_tx_lock) {
        return false;
    }
    if (cred) {
        s_cred = *cred;
    }
    /* The link task never touches flash (credentials are copied above), so its
     * stack can live in PSRAM; internal RAM is kept for Wi-Fi, DMA and the
     * WebSocket tasks. */
    if (xTaskCreatePinnedToCoreWithCaps(sender_task, "nf_send", 8192, NULL, 6, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return false;
    }
    return xTaskCreatePinnedToCoreWithCaps(link_task, "nf_link", 12288, NULL, 5, NULL, 0,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

void nf_link_restart(const nf_credentials_t *cred)
{
    if (cred) {
        s_cred = *cred;
    }
    s_restart = true;
}

bool nf_link_send_frame(const char *frame_json)
{
    if (!frame_json || !s_data.connected || !s_outbox) {
        return false;
    }
    size_t n = strlen(frame_json) + 1U;
    char *copy = ps_malloc(n);
    if (!copy) {
        return false;
    }
    memcpy(copy, frame_json, n);
    if (xQueueSend(s_outbox, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "uplink queue full; dropping a frame");
        free(copy);
        return false;
    }
    return true;
}

char *nf_link_poll(void)
{
    char *msg = NULL;
    if (s_inbox && xQueueReceive(s_inbox, &msg, 0) == pdTRUE) {
        return msg;
    }
    return NULL;
}

void nf_link_free(char *msg)
{
    free(msg);
}

void nf_link_get_status(nf_link_status_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

bool nf_link_request_mode(const char *mode)
{
    if (!s_lock || !mode || (strcmp(mode, "chat") != 0 && strcmp(mode, "agent") != 0)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_status.attached;
    if (ok) {
        snprintf(s_pending_mode, sizeof(s_pending_mode), "%s", mode);
    }
    xSemaphoreGive(s_lock);
    return ok;
}
