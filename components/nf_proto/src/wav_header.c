#include "nf_proto.h"
#include <string.h>

static uint16_t read_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

bool nf_wav_parse_header(const uint8_t *buf, size_t len, nf_wav_info_t *out_info)
{
    if (!buf || !out_info || len < NF_WAV_MIN_HEADER_SIZE) {
        return false;
    }
    /* Check "RIFF" at 0..4 and "WAVE" at 8..12 */
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool found_fmt = false;
    uint16_t audio_format = 0U;
    uint16_t num_channels = 0U;
    uint32_t sample_rate = 0U;
    uint16_t bits_per_sample = 0U;

    size_t pos = 12U;
    while (pos + 8U <= len) {
        const uint8_t *chunk_id = buf + pos;
        uint32_t chunk_size = read_u32_le(buf + pos + 4U);
        size_t chunk_data_pos = pos + 8U;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16U || chunk_data_pos + 16U > len) {
                return false;
            }
            audio_format = read_u16_le(buf + chunk_data_pos + 0U);
            num_channels = read_u16_le(buf + chunk_data_pos + 2U);
            sample_rate = read_u32_le(buf + chunk_data_pos + 4U);
            bits_per_sample = read_u16_le(buf + chunk_data_pos + 14U);
            found_fmt = true;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            if (!found_fmt) {
                return false;
            }
            /* Only PCM (format=1), 16-bit, 1 or 2 channels, 8kHz..48kHz supported */
            if (audio_format != 1U || bits_per_sample != 16U) {
                return false;
            }
            if (num_channels < 1U || num_channels > 2U) {
                return false;
            }
            if (sample_rate < 8000U || sample_rate > 48000U) {
                return false;
            }
            out_info->sample_rate = sample_rate;
            out_info->num_channels = num_channels;
            out_info->bits_per_sample = bits_per_sample;
            out_info->data_size = chunk_size;
            out_info->pcm_offset = chunk_data_pos;
            return true;
        }

        /* Advance to next RIFF subchunk (word-aligned) */
        size_t step = (size_t)chunk_size + ((chunk_size & 1U) ? 1U : 0U);
        if (chunk_data_pos + step <= pos) {
            return false; /* overflow guard */
        }
        pos = chunk_data_pos + step;
    }

    return false;
}
