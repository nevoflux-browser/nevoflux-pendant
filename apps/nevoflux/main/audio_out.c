#include "nevoflux_app.h"
#include <string.h>

void nf_asset_stream_init(nf_asset_stream_t *st)
{
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

void nf_asset_stream_start(nf_asset_stream_t *st, const char *asset_id, uint64_t total_size)
{
    if (!st || !asset_id || asset_id[0] == '\0') {
        return;
    }
    memset(st, 0, sizeof(*st));
    strncpy(st->asset_id, asset_id, NFM1_MAX_ID_LEN);
    st->asset_id[NFM1_MAX_ID_LEN] = '\0';
    st->total_size = total_size;
    st->active = true;
}

void nf_asset_stream_stop(nf_asset_stream_t *st)
{
    if (!st) {
        return;
    }
    st->active = false;
    st->in_flight = 0U;
}

bool nf_asset_stream_next_pull(nf_asset_stream_t *st,
                               bool media_channel_open,
                               char *out_json,
                               size_t out_cap)
{
    if (!st || !st->active || st->eof_reached || !out_json) {
        return false;
    }
    if (st->in_flight >= NF_ASSET_MAX_IN_FLIGHT) {
        return false;
    }
    if (st->total_size > 0ULL && st->next_request_offset >= st->total_size) {
        return false;
    }

    uint32_t pull_len = NF_ASSET_PULL_CHUNK_BYTES;
    if (st->total_size > 0ULL &&
        (st->next_request_offset + (uint64_t)pull_len) > st->total_size) {
        pull_len = (uint32_t)(st->total_size - st->next_request_offset);
    }

    if (nf_format_asset_pull_msg(st->asset_id,
                                 st->next_request_offset,
                                 pull_len,
                                 media_channel_open,
                                 out_json,
                                 out_cap) < 0) {
        return false;
    }

    st->next_request_offset += (uint64_t)pull_len;
    st->in_flight++;
    return true;
}

bool nf_asset_stream_on_nfm1(nf_asset_stream_t *st,
                             const nfm1_frame_t *frame,
                             const uint8_t **out_pcm_ptr,
                             size_t *out_pcm_len)
{
    if (!st || !st->active || !frame || !out_pcm_ptr || !out_pcm_len) {
        return false;
    }
    if (strcmp(frame->id, st->asset_id) != 0) {
        return false;
    }

    if (st->in_flight > 0U) {
        st->in_flight--;
    }
    if (frame->eof) {
        st->eof_reached = true;
    }
    st->bytes_received += (uint64_t)frame->payload_len;
    if (st->total_size > 0ULL && st->bytes_received >= st->total_size) {
        st->eof_reached = true;
    }

    /* First chunk (offset == 0): parse 44-byte WAV header and dynamically switch ES8311 sample rate */
    if (frame->offset == 0ULL && !st->wav_header_parsed) {
        nf_wav_info_t wav_info;
        if (!nf_wav_parse_header(frame->payload, frame->payload_len, &wav_info)) {
            return false;
        }
        st->wav_header_parsed = true;
        st->sample_rate = wav_info.sample_rate;
        (void)bsp_audio_set_play_sample_rate(wav_info.sample_rate);

        *out_pcm_ptr = frame->payload + wav_info.pcm_offset;
        *out_pcm_len = frame->payload_len - wav_info.pcm_offset;
        return true;
    }

    *out_pcm_ptr = frame->payload;
    *out_pcm_len = frame->payload_len;
    return true;
}
