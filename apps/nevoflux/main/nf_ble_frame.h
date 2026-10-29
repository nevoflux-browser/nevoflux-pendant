#pragma once
/*
 * Records on the BLE link to the head. The GATT link is one ordered byte stream
 * each way (central writes to RX, pendant notifies on TX), cut into ATT packets
 * anywhere; on it, records follow one another:
 *
 *   type (1) | length (3, little-endian) | payload
 *
 *   NF_BLE_REC_CTL / NF_BLE_REC_DATA: one sealed frame of that channel, exactly
 *     the bytes a binary WebSocket message carries on the LAN or the relay.
 *   NF_BLE_REC_HELLO (pendant -> head, first): {"c":"<control id>","d":"<data id>"},
 *     what `?c=` says on a socket dial; the head answers nothing.
 *
 * The parts here are pure so they are tested on the host.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NF_BLE_REC_CTL    0x00
#define NF_BLE_REC_DATA   0x01
#define NF_BLE_REC_HELLO  0x7F
#define NF_BLE_REC_HDR    4U
#define NF_BLE_REC_MAX    (2U * 1024U * 1024U)

void nf_ble_rec_header(uint8_t type, uint32_t len, uint8_t out[NF_BLE_REC_HDR]);

/* A whole record; the receiver owns buf (from the reassembler's alloc) and frees it */
typedef void (*nf_ble_rec_cb_t)(uint8_t type, uint8_t *buf, size_t len, void *ctx);

typedef struct {
    void *(*alloc)(size_t n);
    void (*release)(void *p);
    uint8_t  hdr[NF_BLE_REC_HDR];
    size_t   hdr_n;
    uint8_t *buf;
    size_t   len;
    size_t   got;
} nf_ble_rx_t;

void nf_ble_rx_init(nf_ble_rx_t *rx, void *(*alloc)(size_t), void (*release)(void *));
/* Drop a half-received record (the link went down) */
void nf_ble_rx_reset(nf_ble_rx_t *rx);
/* Feed bytes as they arrive; whole records go to cb. False: a record over NF_BLE_REC_MAX
 * or out of memory — the stream cannot be followed any more, drop the link. */
bool nf_ble_rx_feed(nf_ble_rx_t *rx, const uint8_t *p, size_t n, nf_ble_rec_cb_t cb, void *ctx);

/* The 4 bytes after the service UUID in the advertised service data: the data
 * channel's tag (the 8 hex digits mDNS lists in TXT `ch`) as bytes. False if malformed. */
bool nf_ble_tag_bytes(const char *hex_tag, uint8_t out[4]);
