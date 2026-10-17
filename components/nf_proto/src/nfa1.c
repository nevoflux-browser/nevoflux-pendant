#include "nf_proto.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_u16_be(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)((v >> 8) & 0xFFU);
    dst[1] = (uint8_t)(v & 0xFFU);
}

static void write_u32_be(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)((v >> 24) & 0xFFU);
    dst[1] = (uint8_t)((v >> 16) & 0xFFU);
    dst[2] = (uint8_t)((v >> 8) & 0xFFU);
    dst[3] = (uint8_t)(v & 0xFFU);
}

static void write_u64_be(uint8_t *dst, uint64_t v)
{
    write_u32_be(dst, (uint32_t)(v >> 32));
    write_u32_be(dst + 4, (uint32_t)(v & 0xFFFFFFFFULL));
}

static uint16_t read_u16_be(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | (uint16_t)src[1]);
}

static uint32_t read_u32_be(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8)  |
           ((uint32_t)src[3]);
}

static uint64_t read_u64_be(const uint8_t *src)
{
    return ((uint64_t)read_u32_be(src) << 32) | (uint64_t)read_u32_be(src + 4);
}

void nf_utterance_gen_init(nf_utterance_gen_t *gen, uint32_t boot_nonce)
{
    if (!gen) {
        return;
    }
    gen->boot_nonce = boot_nonce;
    gen->counter = 0U;
}

uint64_t nf_utterance_gen_next(nf_utterance_gen_t *gen)
{
    if (!gen) {
        return 0ULL;
    }
    uint32_t ctr = gen->counter++;
    return ((uint64_t)gen->boot_nonce << 32) | (uint64_t)ctr;
}

int nf_utterance_id_to_str(uint64_t utterance_id, char *out, size_t out_cap)
{
    if (!out || out_cap == 0U) {
        return -1;
    }
    int n = snprintf(out, out_cap, "%" PRIu64, utterance_id);
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return n;
}

bool nf_utterance_id_from_str(const char *str, uint64_t *out_id)
{
    if (!str || !out_id || *str == '\0') {
        return false;
    }
    uint64_t val = 0ULL;
    for (const char *p = str; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(*p - '0');
        if (val > (UINT64_MAX - digit) / 10ULL) {
            return false;
        }
        val = val * 10ULL + digit;
    }
    *out_id = val;
    return true;
}

bool nfa1_encode_header(const nfa1_header_t *hdr, uint8_t out_buf[NFA1_HEADER_SIZE])
{
    if (!hdr || !out_buf) {
        return false;
    }
    if ((hdr->flags & ~NFA1_FLAG_VALID_MASK) != 0U) {
        return false;
    }
    if (hdr->format != NFA1_FORMAT_PCM16_16K) {
        return false;
    }
    if (hdr->reserved != 0U) {
        return false;
    }

    out_buf[0] = 0x4EU; /* 'N' */
    out_buf[1] = 0x46U; /* 'F' */
    out_buf[2] = 0x41U; /* 'A' */
    out_buf[3] = 0x31U; /* '1' */
    out_buf[4] = hdr->flags;
    out_buf[5] = hdr->format;
    write_u16_be(out_buf + 6, 0x0000U);
    write_u64_be(out_buf + 8, hdr->utterance_id);
    write_u32_be(out_buf + 16, hdr->chunk_seq);
    write_u32_be(out_buf + 20, hdr->frame_seq);
    return true;
}

bool nfa1_decode_header(const uint8_t *buf, size_t len, nfa1_header_t *out_hdr)
{
    if (!buf || !out_hdr || len < NFA1_HEADER_SIZE) {
        return false;
    }
    if (buf[0] != 0x4EU || buf[1] != 0x46U || buf[2] != 0x41U || buf[3] != 0x31U) {
        return false;
    }
    uint8_t flags = buf[4];
    uint8_t format = buf[5];
    uint16_t reserved = read_u16_be(buf + 6);
    if ((flags & ~NFA1_FLAG_VALID_MASK) != 0U) {
        return false;
    }
    if (format != NFA1_FORMAT_PCM16_16K || reserved != 0U) {
        return false;
    }

    out_hdr->flags = flags;
    out_hdr->format = format;
    out_hdr->reserved = reserved;
    out_hdr->utterance_id = read_u64_be(buf + 8);
    out_hdr->chunk_seq = read_u32_be(buf + 16);
    out_hdr->frame_seq = read_u32_be(buf + 20);
    return true;
}

size_t nfa1_pack_frame(const nfa1_header_t *hdr,
                       const uint8_t *pcm_bytes,
                       size_t pcm_len,
                       uint8_t *out_buf,
                       size_t out_cap)
{
    if (!hdr || !out_buf) {
        return 0U;
    }
    if (pcm_len > 0U && !pcm_bytes) {
        return 0U;
    }
    /* Pure CANCEL frames must carry 0 bytes of PCM */
    if ((hdr->flags & NFA1_FLAG_CANCEL) != 0U && pcm_len != 0U) {
        return 0U;
    }
    if (out_cap < NFA1_HEADER_SIZE + pcm_len) {
        return 0U;
    }
    if (!nfa1_encode_header(hdr, out_buf)) {
        return 0U;
    }
    if (pcm_len > 0U) {
        memcpy(out_buf + NFA1_HEADER_SIZE, pcm_bytes, pcm_len);
    }
    return NFA1_HEADER_SIZE + pcm_len;
}
