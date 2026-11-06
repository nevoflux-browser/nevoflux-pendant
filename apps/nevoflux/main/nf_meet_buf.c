#include "nf_meet_buf.h"

#include <stdio.h>
#include <string.h>

void nf_meet_buf_init(nf_meet_buf_t *b, uint8_t *mem, size_t cap)
{
    memset(b, 0, sizeof(*b));
    b->mem = mem;
    b->cap = mem ? cap : 0U;
}

bool nf_meet_buf_empty(const nf_meet_buf_t *b)
{
    return b->records == 0U;
}

static void copy_in(nf_meet_buf_t *b, size_t at, const uint8_t *p, size_t n)
{
    at %= b->cap;
    size_t first = b->cap - at < n ? b->cap - at : n;
    memcpy(b->mem + at, p, first);
    memcpy(b->mem, p + first, n - first);
}

static void copy_out(const nf_meet_buf_t *b, size_t at, uint8_t *p, size_t n)
{
    at %= b->cap;
    size_t first = b->cap - at < n ? b->cap - at : n;
    memcpy(p, b->mem + at, first);
    memcpy(p + first, b->mem, n - first);
}

static void header_at(const nf_meet_buf_t *b, size_t at, uint8_t *type, size_t *len, uint32_t *offset_ms)
{
    uint8_t h[NF_MEET_BUF_HDR];
    copy_out(b, at, h, sizeof(h));
    *type = h[0];
    *len = (size_t)h[1] | (size_t)h[2] << 8;
    *offset_ms = (uint32_t)h[3] | (uint32_t)h[4] << 8 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 24;
}

static void note_gap(nf_meet_buf_t *b, uint32_t offset_ms)
{
    if (b->gap_count > 0U) {
        uint32_t *last = b->gaps[b->gap_count - 1U];
        if (offset_ms <= last[1] + NF_MEET_BUF_GAP_JOIN_MS || b->gap_count == NF_MEET_BUF_MAX_GAPS) {
            if (offset_ms > last[1]) {
                last[1] = offset_ms;
            }
            return;
        }
    }
    b->gaps[b->gap_count][0] = offset_ms;
    b->gaps[b->gap_count][1] = offset_ms;
    b->gap_count++;
}

static void drop_oldest(nf_meet_buf_t *b, bool as_gap)
{
    uint8_t type;
    size_t len;
    uint32_t off;
    header_at(b, b->head, &type, &len, &off);
    if (as_gap) {
        note_gap(b, off);
    }
    b->head = (b->head + NF_MEET_BUF_HDR + len) % b->cap;
    b->used -= NF_MEET_BUF_HDR + len;
    b->records--;
}

bool nf_meet_buf_put(nf_meet_buf_t *b, uint8_t type, uint32_t offset_ms, const uint8_t *payload, size_t len)
{
    size_t need = NF_MEET_BUF_HDR + len;
    if (!b->mem || len > 0xFFFFU || need > b->cap) {
        return false;
    }
    while (b->cap - b->used < need) {
        drop_oldest(b, true);
    }
    uint8_t h[NF_MEET_BUF_HDR] = {type, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)offset_ms,
                                  (uint8_t)(offset_ms >> 8), (uint8_t)(offset_ms >> 16), (uint8_t)(offset_ms >> 24)};
    size_t tail = b->head + b->used;
    copy_in(b, tail, h, sizeof(h));
    if (len > 0U) {
        copy_in(b, tail + NF_MEET_BUF_HDR, payload, len);
    }
    b->used += need;
    b->records++;
    return true;
}

long nf_meet_buf_peek(const nf_meet_buf_t *b, uint8_t *type, uint32_t *offset_ms, uint8_t *out, size_t cap)
{
    if (b->records == 0U) {
        return -1;
    }
    size_t len;
    header_at(b, b->head, type, &len, offset_ms);
    copy_out(b, b->head + NF_MEET_BUF_HDR, out, len < cap ? len : cap);
    return (long)len;
}

void nf_meet_buf_pop(nf_meet_buf_t *b)
{
    if (b->records > 0U) {
        drop_oldest(b, false);
    }
}

size_t nf_meet_buf_gaps_json(const nf_meet_buf_t *b, char *out, size_t cap)
{
    size_t n = 0U;
    if (cap < 3U) {
        return 0U;
    }
    out[n++] = '[';
    for (size_t i = 0; i < b->gap_count; ++i) {
        int w = snprintf(out + n, cap - n, "%s[%u,%u]", i ? "," : "", (unsigned)b->gaps[i][0], (unsigned)b->gaps[i][1]);
        if (w < 0 || (size_t)w >= cap - n - 1U) {
            break;
        }
        n += (size_t)w;
    }
    out[n++] = ']';
    out[n] = '\0';
    return n;
}
