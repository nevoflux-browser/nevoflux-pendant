#include "ble_prov.h"
#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_prov";

#define BLE_PROV_LINE_MAX 1024U

/* Nordic UART Service (little-endian byte order as NimBLE expects) */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static char s_name[24] = "";
static uint8_t s_own_addr_type;
static uint16_t s_tx_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_tx_subscribed;
static volatile uint16_t s_mtu = 23;
static volatile bool s_enabled;
static volatile bool s_synced;
static MessageBufferHandle_t s_rx_lines;

/* Request accumulator (NimBLE host task only). A request ends at '\n' or when
 * a top-level JSON object closes, so clients that omit the newline still work. */
static char s_acc[BLE_PROV_LINE_MAX];
static size_t s_acc_len;
static int s_depth;
static bool s_in_str;
static bool s_esc;

static void acc_reset(void)
{
    s_acc_len = 0;
    s_depth = 0;
    s_in_str = false;
    s_esc = false;
}

static void acc_emit(void)
{
    if (s_acc_len > 0 && s_rx_lines) {
        if (xMessageBufferSend(s_rx_lines, s_acc, s_acc_len, 0) != s_acc_len) {
            ESP_LOGW(TAG, "request queue full, dropping %u bytes", (unsigned)s_acc_len);
        }
    }
    acc_reset();
}

static void acc_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            acc_emit();
            continue;
        }
        if (s_acc_len == 0 && (c == ' ' || c == '\t')) {
            continue;
        }
        if (s_acc_len >= BLE_PROV_LINE_MAX - 1U) {
            ESP_LOGW(TAG, "request longer than %u bytes, dropped", (unsigned)BLE_PROV_LINE_MAX);
            acc_reset();
            continue;
        }
        s_acc[s_acc_len++] = c;
        if (s_in_str) {
            if (s_esc) {
                s_esc = false;
            } else if (c == '\\') {
                s_esc = true;
            } else if (c == '"') {
                s_in_str = false;
            }
        } else if (c == '"') {
            s_in_str = true;
        } else if (c == '{') {
            s_depth++;
        } else if (c == '}' && s_depth > 0 && --s_depth == 0) {
            acc_emit();
        }
    }
}

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[256];
    struct os_mbuf *om = ctxt->om;
    while (om) {
        uint16_t off = 0;
        while (off < om->om_len) {
            uint16_t n = (uint16_t)(om->om_len - off);
            if (n > sizeof(buf)) {
                n = sizeof(buf);
            }
            memcpy(buf, om->om_data + off, n);
            acc_feed(buf, n);
            off = (uint16_t)(off + n);
        }
        om = SLIST_NEXT(om, om_next);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = gatt_access_cb,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static void advertise(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0 && !s_enabled) {
            (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_mtu = 23;
            acc_reset();
            ESP_LOGI(TAG, "client connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "client disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_tx_subscribed = false;
        acc_reset();
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_tx_subscribed = event->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (!s_enabled || !s_synced || s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    /* 128-bit UUID does not fit next to the name in 31 bytes: put it in the scan response */
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = &s_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan rsp rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE identity address");
        return;
    }
    uint8_t addr[6] = {0};
    (void)ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    snprintf(s_name, sizeof(s_name), "NevoFlux-%02X%02X", addr[1], addr[0]);
    (void)ble_svc_gap_device_name_set(s_name);
    s_synced = true;
    ESP_LOGI(TAG, "ready as \"%s\" (Nordic UART Service, advertising %s)", s_name,
             s_enabled ? "on" : "off until enabled");
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool launcher_ble_prov_init(void)
{
    if (s_rx_lines) {
        return true;
    }
    s_rx_lines = xMessageBufferCreate(2U * BLE_PROV_LINE_MAX + 16U);
    if (!s_rx_lines) {
        return false;
    }
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_gatt_svcs) != 0 || ble_gatts_add_svcs(s_gatt_svcs) != 0) {
        ESP_LOGE(TAG, "GATT service registration failed");
        return false;
    }
    (void)ble_att_set_preferred_mtu(247);
    nimble_port_freertos_init(host_task);
    return true;
}

void launcher_ble_prov_set_enabled(bool enabled)
{
    if (!s_rx_lines || s_enabled == enabled) {
        return;
    }
    s_enabled = enabled;
    if (enabled) {
        ESP_LOGI(TAG, "BLE provisioning opened");
        advertise();
        return;
    }
    ESP_LOGI(TAG, "BLE provisioning closed");
    (void)ble_gap_adv_stop();
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool launcher_ble_prov_is_enabled(void)
{
    return s_enabled;
}

bool launcher_ble_prov_is_connected(void)
{
    return s_enabled && s_conn != BLE_HS_CONN_HANDLE_NONE;
}

bool launcher_ble_prov_poll(char *out, size_t cap)
{
    if (!s_rx_lines || !out || cap < 2U) {
        return false;
    }
    size_t n = xMessageBufferReceive(s_rx_lines, out, cap - 1U, 0);
    if (n == 0) {
        return false;
    }
    if (!s_enabled) {
        return false; /* request raced with closing the window: drop it */
    }
    out[n] = '\0';
    return true;
}

void launcher_ble_prov_send(const char *resp)
{
    uint16_t conn = s_conn;
    if (!resp || conn == BLE_HS_CONN_HANDLE_NONE || !s_tx_subscribed) {
        return;
    }
    char line[BLE_PROV_LINE_MAX];
    int len = snprintf(line, sizeof(line), "%s\n", resp);
    if (len <= 0) {
        return;
    }
    if ((size_t)len >= sizeof(line)) {
        len = (int)sizeof(line) - 1;
    }
    size_t chunk = (s_mtu > 3U) ? (size_t)(s_mtu - 3U) : 20U;
    for (int off = 0; off < len;) {
        size_t n = (size_t)(len - off);
        if (n > chunk) {
            n = chunk;
        }
        int rc = BLE_HS_ENOMEM;
        for (int attempt = 0; attempt < 20 && rc != 0; ++attempt) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(line + off, (uint16_t)n);
            rc = om ? ble_gatts_notify_custom(conn, s_tx_handle, om) : BLE_HS_ENOMEM;
            if (rc == BLE_HS_ENOMEM) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else if (rc != 0) {
                ESP_LOGW(TAG, "notify failed rc=%d", rc);
                return;
            }
        }
        if (rc != 0) {
            return;
        }
        off += (int)n;
    }
}

const char *launcher_ble_prov_name(void)
{
    return s_name;
}

#else /* Host build: no BLE */

void launcher_ble_prov_set_enabled(bool enabled)
{
    (void)enabled;
}

bool launcher_ble_prov_is_enabled(void)
{
    return false;
}

bool launcher_ble_prov_is_connected(void)
{
    return false;
}

bool launcher_ble_prov_init(void)
{
    return false;
}

bool launcher_ble_prov_poll(char *out, size_t cap)
{
    (void)out;
    (void)cap;
    return false;
}

void launcher_ble_prov_send(const char *resp)
{
    (void)resp;
}

const char *launcher_ble_prov_name(void)
{
    return "";
}

#endif /* ESP_PLATFORM */
