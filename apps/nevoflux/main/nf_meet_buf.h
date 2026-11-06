#pragma once
/*
 * What a meeting records while the head cannot be reached: a ring of records
 * in PSRAM, sent up in order once it can. Full, it drops the oldest records and
 * remembers the time they covered, so the head can mark the hole in the record.
 *
 *   type (1) | length (2, LE) | offset_ms (4, LE) | payload
 *
 * The record types are the caller's; offset_ms is when in the meeting the
 * record was made. Pure, so it is tested on the host.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NF_MEET_BUF_HDR       7U
#define NF_MEET_BUF_MAX_GAPS  16U
/* Dropped records this close together are one hole */
#define NF_MEET_BUF_GAP_JOIN_MS 2000U

typedef struct {
    uint8_t *mem;
    size_t   cap;
    size_t   head; /* oldest record */
    size_t   used;
    size_t   records;
    uint32_t gaps[NF_MEET_BUF_MAX_GAPS][2];
    size_t   gap_count;
} nf_meet_buf_t;

void nf_meet_buf_init(nf_meet_buf_t *b, uint8_t *mem, size_t cap);
bool nf_meet_buf_empty(const nf_meet_buf_t *b);
/* Append one record, dropping the oldest to make room. False if it can never fit. */
bool nf_meet_buf_put(nf_meet_buf_t *b, uint8_t type, uint32_t offset_ms, const uint8_t *payload, size_t len);
/* The oldest record: its payload length (copied into out, up to cap), or -1 if empty */
long nf_meet_buf_peek(const nf_meet_buf_t *b, uint8_t *type, uint32_t *offset_ms, uint8_t *out, size_t cap);
void nf_meet_buf_pop(nf_meet_buf_t *b);
/* Holes so far as JSON, `[[from_ms,to_ms],...]`; the length written */
size_t nf_meet_buf_gaps_json(const nf_meet_buf_t *b, char *out, size_t cap);
