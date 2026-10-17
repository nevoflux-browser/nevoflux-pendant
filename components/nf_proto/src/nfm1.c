#include "nf_proto.h"
#include <string.h>

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

static bool is_safe_asset_id_bytes(const uint8_t *id_bytes, uint8_t id_len)
{
    if (id_len == 0U || id_len > NFM1_MAX_ID_LEN) {
        return false;
    }
    for (uint8_t i = 0U; i < id_len; ++i) {
        uint8_t c = id_bytes[i];
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  (c == '_') || (c == '-');
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool nfm1_is_media_frame(const uint8_t *buf, size_t len)
{
    if (!buf || len < 4U) {
        return false;
    }
    return buf[0] == 0x4EU && buf[1] == 0x46U && buf[2] == 0x4DU && buf[3] == 0x31U;
}

bool nfm1_decode_frame(const uint8_t *buf, size_t len, nfm1_frame_t *out_frame)
{
    if (!buf || !out_frame || !nfm1_is_media_frame(buf, len) || len < NFM1_FIXED_HEADER_SIZE) {
        return false;
    }

    uint8_t id_len = buf[12];
    size_t id_end = 13U + (size_t)id_len;
    if (len < id_end + 9U) {
        return false;
    }
    if (!is_safe_asset_id_bytes(buf + 13, id_len)) {
        return false;
    }

    memset(out_frame, 0, sizeof(*out_frame));
    memcpy(out_frame->id, buf + 13, id_len);
    out_frame->id[id_len] = '\0';
    out_frame->id_len = id_len;

    out_frame->offset = read_u64_be(buf + id_end);
    out_frame->flags = buf[id_end + 8U];
    out_frame->eof = (out_frame->flags & NFM1_FLAG_EOF) != 0U;

    if ((out_frame->flags & NFM1_FLAG_NO_SEQ) != 0U) {
        out_frame->has_seq = false;
        out_frame->seq = 0ULL;
    } else {
        out_frame->has_seq = true;
        out_frame->seq = read_u64_be(buf + 4);
    }

    size_t payload_offset = id_end + 9U;
    out_frame->payload = buf + payload_offset;
    out_frame->payload_len = len - payload_offset;
    return true;
}

size_t nfm1_encode_frame(const nfm1_frame_t *frame, uint8_t *out_buf, size_t out_cap)
{
    if (!frame || !out_buf) {
        return 0U;
    }
    size_t id_len = strlen(frame->id);
    if (id_len == 0U || id_len > NFM1_MAX_ID_LEN) {
        return 0U;
    }
    if (!is_safe_asset_id_bytes((const uint8_t *)frame->id, (uint8_t)id_len)) {
        return 0U;
    }
    if (frame->payload_len > 0U && !frame->payload) {
        return 0U;
    }

    size_t total_len = NFM1_FIXED_HEADER_SIZE + id_len + frame->payload_len;
    if (out_cap < total_len) {
        return 0U;
    }

    out_buf[0] = 0x4EU; /* 'N' */
    out_buf[1] = 0x46U; /* 'F' */
    out_buf[2] = 0x4DU; /* 'M' */
    out_buf[3] = 0x31U; /* '1' */

    uint8_t flags = frame->flags;
    if (frame->eof) {
        flags |= NFM1_FLAG_EOF;
    }
    if (!frame->has_seq) {
        flags |= NFM1_FLAG_NO_SEQ;
    }

    write_u64_be(out_buf + 4, frame->has_seq ? frame->seq : 0ULL);
    out_buf[12] = (uint8_t)id_len;
    memcpy(out_buf + 13, frame->id, id_len);

    size_t id_end = 13U + id_len;
    write_u64_be(out_buf + id_end, frame->offset);
    out_buf[id_end + 8U] = flags;

    if (frame->payload_len > 0U) {
        memcpy(out_buf + id_end + 9U, frame->payload, frame->payload_len);
    }
    return total_len;
}
