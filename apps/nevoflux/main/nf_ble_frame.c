#include "nf_ble_frame.h"

#include <string.h>

void nf_ble_rec_header(uint8_t type, uint32_t len, uint8_t out[NF_BLE_REC_HDR])
{
    out[0] = type;
    out[1] = (uint8_t)len;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)(len >> 16);
}

void nf_ble_rx_init(nf_ble_rx_t *rx, void *(*alloc)(size_t), void (*release)(void *))
{
    memset(rx, 0, sizeof(*rx));
    rx->alloc = alloc;
    rx->release = release;
}

void nf_ble_rx_reset(nf_ble_rx_t *rx)
{
    if (rx->buf) {
        rx->release(rx->buf);
    }
    rx->buf = NULL;
    rx->hdr_n = 0;
    rx->len = 0;
    rx->got = 0;
}

bool nf_ble_rx_feed(nf_ble_rx_t *rx, const uint8_t *p, size_t n, nf_ble_rec_cb_t cb, void *ctx)
{
    for (;;) {
        if (rx->hdr_n < NF_BLE_REC_HDR) {
            if (n == 0) {
                return true;
            }
            rx->hdr[rx->hdr_n++] = *p++;
            --n;
            if (rx->hdr_n < NF_BLE_REC_HDR) {
                continue;
            }
            rx->len = (size_t)rx->hdr[1] | (size_t)rx->hdr[2] << 8 | (size_t)rx->hdr[3] << 16;
            if (rx->len > NF_BLE_REC_MAX) {
                nf_ble_rx_reset(rx);
                return false;
            }
            rx->buf = rx->alloc(rx->len + 1U); /* +1: room for a NUL after text */
            if (!rx->buf) {
                nf_ble_rx_reset(rx);
                return false;
            }
            rx->got = 0;
        }
        size_t take = rx->len - rx->got;
        if (take > n) {
            take = n;
        }
        if (take > 0) {
            memcpy(rx->buf + rx->got, p, take);
        }
        rx->got += take;
        p += take;
        n -= take;
        if (rx->got < rx->len) {
            return true; /* the rest comes in later packets */
        }
        uint8_t *buf = rx->buf;
        buf[rx->len] = 0;
        rx->buf = NULL;
        rx->hdr_n = 0;
        cb(rx->hdr[0], buf, rx->len, ctx);
    }
}

static int hex_nibble(char c)
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

bool nf_ble_tag_bytes(const char *hex_tag, uint8_t out[4])
{
    if (strlen(hex_tag) != 8) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        int hi = hex_nibble(hex_tag[2 * i]);
        int lo = hi < 0 ? -1 : hex_nibble(hex_tag[2 * i + 1]);
        if (lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}
