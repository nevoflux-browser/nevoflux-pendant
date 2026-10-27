#include "nf_proto.h"
#include <string.h>

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t nf_b64_encoded_len(size_t raw_len)
{
    return ((raw_len + 2U) / 3U) * 4U;
}

size_t nf_b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    size_t need = nf_b64_encoded_len(in_len);
    if (!out || out_cap < need + 1U || (in_len > 0U && !in)) {
        return 0U;
    }
    size_t o = 0U;
    size_t i = 0U;
    for (; i + 2U < in_len; i += 3U) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1U] << 8) | in[i + 2U];
        out[o++] = k_b64[(v >> 18) & 0x3FU];
        out[o++] = k_b64[(v >> 12) & 0x3FU];
        out[o++] = k_b64[(v >> 6) & 0x3FU];
        out[o++] = k_b64[v & 0x3FU];
    }
    if (i < in_len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1U < in_len) {
            v |= (uint32_t)in[i + 1U] << 8;
        }
        out[o++] = k_b64[(v >> 18) & 0x3FU];
        out[o++] = k_b64[(v >> 12) & 0x3FU];
        out[o++] = (i + 1U < in_len) ? k_b64[(v >> 6) & 0x3FU] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

size_t nf_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap, bool *out_ok)
{
    if (out_ok) {
        *out_ok = false;
    }
    if (!in || !out) {
        return 0U;
    }
    uint32_t acc = 0U;
    int bits = 0;
    size_t o = 0U;
    for (size_t i = 0; i < in_len; ++i) {
        char c = in[i];
        if (c == '=') {
            break;
        }
        if (c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
        int v = b64_val(c);
        if (v < 0) {
            return o;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) {
                return o;
            }
            out[o++] = (uint8_t)((acc >> bits) & 0xFFU);
        }
    }
    if (out_ok) {
        *out_ok = true;
    }
    return o;
}

size_t nf_json_escape(const char *in, char *out, size_t out_cap)
{
    if (!out || out_cap == 0U) {
        return 0U;
    }
    size_t o = 0U;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; ++p) {
        char esc[7];
        size_t n;
        switch (*p) {
        case '"':  memcpy(esc, "\\\"", 2); n = 2; break;
        case '\\': memcpy(esc, "\\\\", 2); n = 2; break;
        case '\n': memcpy(esc, "\\n", 2); n = 2; break;
        case '\r': memcpy(esc, "\\r", 2); n = 2; break;
        case '\t': memcpy(esc, "\\t", 2); n = 2; break;
        default:
            if (*p < 0x20U) {
                static const char hex[] = "0123456789abcdef";
                esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = hex[*p >> 4]; esc[5] = hex[*p & 0xFU];
                n = 6;
            } else {
                esc[0] = (char)*p;
                n = 1;
            }
            break;
        }
        if (o + n >= out_cap) {
            break; /* truncate rather than overflow; never split an escape */
        }
        memcpy(out + o, esc, n);
        o += n;
    }
    out[o] = '\0';
    return o;
}

void nf_resampler_init(nf_resampler_t *rs, uint32_t in_rate, uint32_t out_rate)
{
    if (!rs) {
        return;
    }
    memset(rs, 0, sizeof(*rs));
    rs->in_rate = in_rate ? in_rate : out_rate;
    rs->out_rate = out_rate;
}

size_t nf_resample(nf_resampler_t *rs,
                   const int16_t *in,
                   size_t in_samples,
                   int16_t *out,
                   size_t out_cap)
{
    if (!rs || !out || (in_samples > 0U && !in) || rs->out_rate == 0U) {
        return 0U;
    }
    if (rs->in_rate == rs->out_rate) {
        size_t n = (in_samples < out_cap) ? in_samples : out_cap;
        memcpy(out, in, n * sizeof(int16_t));
        return n;
    }
    /*
     * Linear interpolation with a carried phase, so a sentence fed in pieces
     * comes out the same as fed whole. For 24 kHz -> 16 kHz the speech band
     * (< 8 kHz) survives; a 3-tap pre-filter tames the aliasing of what is
     * above it. Phase is in units of 1/out_rate input samples.
     */
    size_t o = 0U;
    for (size_t i = 0; i < in_samples; ++i) {
        int32_t x = in[i];
        /* [1 2 1]/4 low-pass, applied as samples arrive */
        int32_t y = (rs->x1 + 2 * rs->x0 + x) / 4;
        rs->x1 = rs->x0;
        rs->x0 = x;
        int32_t prev = rs->prev;
        rs->prev = y;
        if (!rs->primed) {
            rs->primed = true;
            continue;
        }
        while (rs->phase < rs->out_rate) {
            if (o >= out_cap) {
                return o;
            }
            int32_t v = prev + (int32_t)(((int64_t)(y - prev) * rs->phase) / rs->out_rate);
            out[o++] = (int16_t)v;
            rs->phase += rs->in_rate;
        }
        rs->phase -= rs->out_rate;
    }
    return o;
}

bool nf_opus_next_packet(const uint8_t *buf, size_t len, size_t *offset, const uint8_t **pkt, size_t *pkt_len)
{
    if (!buf || !offset || !pkt || !pkt_len || *offset + 2U > len) {
        return false;
    }
    size_t n = (size_t)buf[*offset] | ((size_t)buf[*offset + 1U] << 8);
    size_t start = *offset + 2U;
    if (n == 0U || n > len - start) {
        return false;
    }
    *pkt = buf + start;
    *pkt_len = n;
    *offset = start + n;
    return true;
}
