#include "nf_ble.h"
#include "nf_ble_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "nf_ble";

#define NF_BLE_RX_DEPTH 32

/* 6e66000x-7c3f-4b8a-9e0d-5f1b2c3d4e5f, little-endian as NimBLE wants it */
#define NF_BLE_UUID(x) BLE_UUID128_INIT(0x5f, 0x4e, 0x3d, 0x2c, 0x1b, 0x5f, 0x0d, 0x9e, \
                                        0x8a, 0x4b, 0x3f, 0x7c, (x), 0x00, 0x66, 0x6e)
static const ble_uuid128_t s_svc_uuid = NF_BLE_UUID(0x01);
static const ble_uuid128_t s_rx_uuid = NF_BLE_UUID(0x02);
static const ble_uuid128_t s_tx_uuid = NF_BLE_UUID(0x03);

typedef struct {
    uint8_t  type;
    uint8_t *buf;
    size_t   len;
} rec_t;

static nf_ble_on_record_t s_on_record;
static QueueHandle_t      s_rx_q;
static TaskHandle_t       s_rx_task;
static SemaphoreHandle_t  s_tx_lock;
static nf_ble_rx_t        s_rx;
static char               s_name[24];
static uint8_t            s_svc_data[16 + 4];
static uint8_t            s_own_addr_type;
static uint16_t           s_tx_handle;
static volatile uint16_t  s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool      s_subscribed;
static volatile uint16_t  s_mtu = 23;
static volatile bool      s_running;
static volatile bool      s_rx_broken;

static void *ps_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void queue_record(uint8_t type, uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    rec_t r = {.type = type, .buf = buf, .len = len};
    if (xQueueSend(s_rx_q, &r, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "receive queue full; dropping a record");
        free(buf);
    }
}

/* NimBLE's host task: only reassemble here; the records are handled on rx_task */
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (s_rx_broken) {
        return 0;
    }
    for (struct os_mbuf *om = ctxt->om; om; om = SLIST_NEXT(om, om_next)) {
        if (!nf_ble_rx_feed(&s_rx, om->om_data, om->om_len, queue_record, NULL)) {
            ESP_LOGW(TAG, "unreadable stream from the head; disconnecting");
            s_rx_broken = true;
            (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
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
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_mtu = 23;
            s_subscribed = false;
            s_rx_broken = false;
            nf_ble_rx_reset(&s_rx);
            ESP_LOGI(TAG, "head connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "head disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        nf_ble_rx_reset(&s_rx);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "head %s", s_subscribed ? "listening" : "stopped listening");
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU %u", (unsigned)s_mtu);
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (!s_running || s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    /* Advertisement: flags + service data (UUID + tag) so the head can pick us
     * out passively; the name goes in the scan response (31 bytes each). */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.svc_data_uuid128 = s_svc_data;
    fields.svc_data_uuid128_len = sizeof(s_svc_data);
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    (void)ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
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
    ESP_LOGI(TAG, "advertising as \"%s\"", s_name);
    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        rec_t r;
        if (xQueueReceive(s_rx_q, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_on_record) {
            s_on_record(r.type, r.buf, r.len);
        }
        free(r.buf);
    }
}

bool nf_ble_start(const char *tag, nf_ble_on_record_t on_record)
{
    if (s_running) {
        return true;
    }
    uint8_t tag_bytes[4];
    if (!tag || !nf_ble_tag_bytes(tag, tag_bytes)) {
        return false;
    }
    if (!s_rx_q) {
        s_rx_q = xQueueCreate(NF_BLE_RX_DEPTH, sizeof(rec_t));
        s_tx_lock = xSemaphoreCreateMutex();
        if (!s_rx_q || !s_tx_lock ||
            xTaskCreatePinnedToCoreWithCaps(rx_task, "nf_ble_rx", 10240, NULL, 5, &s_rx_task, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            return false;
        }
    }
    s_on_record = on_record;
    memcpy(s_svc_data, s_svc_uuid.value, 16);
    memcpy(s_svc_data + 16, tag_bytes, 4);
    nf_ble_rx_init(&s_rx, ps_alloc, free);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_gatt_svcs) != 0 || ble_gatts_add_svcs(s_gatt_svcs) != 0) {
        ESP_LOGE(TAG, "GATT service registration failed");
        (void)nimble_port_deinit();
        return false;
    }
    (void)ble_att_set_preferred_mtu(247);
    s_running = true;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "on; internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return true;
}

void nf_ble_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    (void)ble_gap_adv_stop();
    xSemaphoreTake(s_tx_lock, portMAX_DELAY); /* no notify in flight while the host goes */
    if (nimble_port_stop() == 0) {
        (void)nimble_port_deinit();
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    xSemaphoreGive(s_tx_lock);
    nf_ble_rx_reset(&s_rx);
    rec_t r;
    while (xQueueReceive(s_rx_q, &r, 0) == pdTRUE) {
        free(r.buf);
    }
    ESP_LOGI(TAG, "off; internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

bool nf_ble_ready(void)
{
    return s_running && s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed;
}

/* One notification of the stream; waits out a full controller queue */
static bool notify_chunk(const uint8_t *p, size_t n, int64_t deadline_us)
{
    while (nf_ble_ready()) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(p, (uint16_t)n);
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om); /* consumes om either way */
            if (rc == 0) {
                return true;
            }
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                return false;
            }
        }
        if (esp_timer_get_time() > deadline_us) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

bool nf_ble_send(uint8_t type, const uint8_t *payload, size_t len, uint32_t timeout_ms)
{
    if (!nf_ble_ready() || len > NF_BLE_REC_MAX) {
        return false;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    static uint8_t chunk[512]; /* under s_tx_lock */
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    size_t room = s_mtu > 3 ? (size_t)s_mtu - 3U : 20U;
    if (room > sizeof(chunk)) {
        room = sizeof(chunk);
    }
    nf_ble_rec_header(type, (uint32_t)len, chunk);
    size_t fill = NF_BLE_REC_HDR;
    size_t off = 0;
    bool ok = true;
    while (ok && (fill > 0 || off < len)) {
        size_t take = len - off < room - fill ? len - off : room - fill;
        if (take > 0) {
            memcpy(chunk + fill, payload + off, take);
        }
        fill += take;
        off += take;
        ok = notify_chunk(chunk, fill, deadline);
        fill = 0;
    }
    xSemaphoreGive(s_tx_lock);
    if (!ok) {
        /* A record cut short corrupts the stream: start the link over */
        uint16_t conn = s_conn;
        if (conn != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGW(TAG, "send stalled; disconnecting");
            (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return ok;
}
