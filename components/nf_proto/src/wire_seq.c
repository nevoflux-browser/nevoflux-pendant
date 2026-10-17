#include "nf_proto.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

nf_relay_text_kind_t nf_relay_classify_text(const char *text, size_t len, int *out_peers)
{
    if (!text || len == 0U) {
        return NF_RELAY_TEXT_UNKNOWN;
    }
    if (len == 4U && memcmp(text, "pong", 4) == 0) {
        return NF_RELAY_TEXT_PONG;
    }

    /* Fast, allocation-free check for `{"k":"peers","n":<int>}` from portal-relay */
    char buf[128];
    if (len >= sizeof(buf)) {
        return NF_RELAY_TEXT_UNKNOWN;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (strstr(buf, "\"k\":\"peers\"") == NULL && strstr(buf, "\"k\": \"peers\"") == NULL) {
        return NF_RELAY_TEXT_UNKNOWN;
    }
    const char *n_key = strstr(buf, "\"n\":");
    if (!n_key) {
        return NF_RELAY_TEXT_UNKNOWN;
    }
    n_key += 4;
    while (*n_key == ' ' || *n_key == '\t') {
        ++n_key;
    }
    if (*n_key < '0' || *n_key > '9') {
        return NF_RELAY_TEXT_UNKNOWN;
    }
    int peers = atoi(n_key);
    if (out_peers) {
        *out_peers = peers;
    }
    return NF_RELAY_TEXT_PEERS;
}

void nf_seq_tracker_init(nf_seq_tracker_t *tracker, uint64_t initial_seq)
{
    if (!tracker) {
        return;
    }
    memset(tracker, 0, sizeof(*tracker));
    tracker->next_expected = initial_seq;
}

void nf_seq_tracker_reset(nf_seq_tracker_t *tracker, uint64_t next_seq)
{
    nf_seq_tracker_init(tracker, next_seq);
}

uint64_t nf_seq_tracker_expected(const nf_seq_tracker_t *tracker)
{
    return tracker ? tracker->next_expected : 0ULL;
}

static nf_seq_slot_t *find_slot_by_seq(nf_seq_tracker_t *tracker, uint64_t seq)
{
    for (size_t i = 0U; i < NF_SEQ_WINDOW_SIZE; ++i) {
        if (tracker->held[i].occupied && tracker->held[i].seq == seq) {
            return &tracker->held[i];
        }
    }
    return NULL;
}

static nf_seq_slot_t *find_free_slot(nf_seq_tracker_t *tracker)
{
    for (size_t i = 0U; i < NF_SEQ_WINDOW_SIZE; ++i) {
        if (!tracker->held[i].occupied) {
            return &tracker->held[i];
        }
    }
    return NULL;
}

bool nf_seq_tracker_receive(nf_seq_tracker_t *tracker,
                            uint64_t seq,
                            const uint8_t *frame_json,
                            size_t frame_len,
                            nf_seq_deliver_cb_t cb,
                            void *user_ctx,
                            bool *out_gap)
{
    if (out_gap) {
        *out_gap = false;
    }
    if (!tracker || !frame_json || frame_len == 0U || frame_len > NF_SEQ_MAX_FRAME_BYTES) {
        return false;
    }

    /* Duplicate or already delivered */
    if (seq < tracker->next_expected) {
        return true;
    }

    /* Out-of-order future frame: buffer in window and signal gap */
    if (seq > tracker->next_expected) {
        if (out_gap) {
            *out_gap = true;
        }
        if (find_slot_by_seq(tracker, seq) != NULL) {
            return true; /* already buffered */
        }
        nf_seq_slot_t *slot = find_free_slot(tracker);
        if (!slot) {
            return false; /* window full */
        }
        slot->occupied = true;
        slot->seq = seq;
        slot->len = frame_len;
        memcpy(slot->data, frame_json, frame_len);
        return true;
    }

    /* seq == tracker->next_expected: deliver immediately, then drain contiguous held frames */
    if (cb) {
        cb(seq, frame_json, frame_len, user_ctx);
    }
    tracker->next_expected += 1ULL;

    for (;;) {
        nf_seq_slot_t *next_slot = find_slot_by_seq(tracker, tracker->next_expected);
        if (!next_slot) {
            break;
        }
        if (cb) {
            cb(next_slot->seq, next_slot->data, next_slot->len, user_ctx);
        }
        next_slot->occupied = false;
        tracker->next_expected += 1ULL;
    }

    return true;
}

int nf_format_resume_msg(uint64_t expected_seq, char *out_buf, size_t out_cap)
{
    if (!out_buf || out_cap == 0U) {
        return -1;
    }
    int n = snprintf(out_buf, out_cap, "{\"k\":\"resume\",\"from\":%" PRIu64 "}", expected_seq);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return n;
}

int nf_format_asset_pull_msg(const char *asset_id,
                             uint64_t offset,
                             uint32_t length,
                             bool media_channel_open,
                             char *out_buf,
                             size_t out_cap)
{
    if (!asset_id || !out_buf || out_cap == 0U) {
        return -1;
    }
    int n = snprintf(
        out_buf,
        out_cap,
        "{\"k\":\"frame\",\"frame\":{\"kind\":\"asset_pull\",\"id\":\"%s\",\"offset\":%" PRIu64
        ",\"length\":%" PRIu32 ",\"binary\":true,\"mediaChannel\":%s}}",
        asset_id,
        offset,
        length,
        media_channel_open ? "true" : "false");
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return n;
}
