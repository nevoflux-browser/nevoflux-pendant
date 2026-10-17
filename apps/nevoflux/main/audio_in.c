#include "nevoflux_app.h"
#include <string.h>

void nf_uplink_queue_init(nf_uplink_queue_t *q)
{
    if (!q) {
        return;
    }
    memset(q, 0, sizeof(*q));
}

void nf_uplink_queue_clear(nf_uplink_queue_t *q)
{
    if (!q) {
        return;
    }
    q->head = 0U;
    q->tail = 0U;
    q->count = 0U;
}

bool nf_uplink_queue_push(nf_uplink_queue_t *q, const uint8_t *frame, size_t len)
{
    if (!q || !frame || len == 0U || len > NF_MAX_NFA1_FRAME_BYTES) {
        return false;
    }

    /* Bounded 500ms queue (25 frames): if full during weak-network stall,
     * drop the oldest frame so stale audio never piles up, while caller's
     * `chunk_seq` continues incrementing so daemon records the gap in `speech_final.gaps`.
     */
    if (q->count == NF_UPLINK_QUEUE_CAPACITY) {
        q->head = (q->head + 1U) % NF_UPLINK_QUEUE_CAPACITY;
        q->count--;
        q->dropped_frames++;
    }

    nf_uplink_slot_t *slot = &q->slots[q->tail];
    slot->len = len;
    memcpy(slot->bytes, frame, len);
    q->tail = (q->tail + 1U) % NF_UPLINK_QUEUE_CAPACITY;
    q->count++;
    return true;
}

bool nf_uplink_queue_pop(nf_uplink_queue_t *q, uint8_t *out_frame, size_t out_cap, size_t *out_len)
{
    if (!q || !out_frame || !out_len || q->count == 0U) {
        return false;
    }
    nf_uplink_slot_t *slot = &q->slots[q->head];
    if (out_cap < slot->len) {
        return false;
    }
    memcpy(out_frame, slot->bytes, slot->len);
    *out_len = slot->len;
    q->head = (q->head + 1U) % NF_UPLINK_QUEUE_CAPACITY;
    q->count--;
    return true;
}
