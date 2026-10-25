#include "nevoflux_app.h"
#include <string.h>

size_t nf_reply_wav_to_16k(const uint8_t *wav, size_t wav_len, int16_t *out, size_t out_cap)
{
    nf_wav_info_t info;
    if (!wav || !out || !nf_wav_parse_header(wav, wav_len, &info)) {
        return 0U;
    }
    if (info.bits_per_sample != 16U || info.num_channels == 0U || info.num_channels > 2U) {
        return 0U;
    }
    size_t avail = wav_len - info.pcm_offset;
    if (info.data_size > 0U && info.data_size < avail) {
        avail = info.data_size;
    }
    const uint8_t *pcm = wav + info.pcm_offset;
    const size_t frame_bytes = 2U * info.num_channels;
    const size_t frames = avail / frame_bytes;

    /*
     * The mic runs at 16 kHz and shares the I2S clock with the speaker, so the
     * answer is brought to 16 kHz rather than the bus to the answer's rate:
     * the mic stays open while it plays, and nothing clicks on a rate switch.
     */
    nf_resampler_t rs;
    nf_resampler_init(&rs, info.sample_rate, 16000U);
    int16_t block[256];
    size_t o = 0U;
    for (size_t f = 0; f < frames && o < out_cap;) {
        size_t n = 0U;
        for (; n < sizeof(block) / sizeof(block[0]) && f < frames; ++n, ++f) {
            const uint8_t *s = pcm + f * frame_bytes;
            int32_t v = (int16_t)((uint16_t)s[0] | ((uint16_t)s[1] << 8));
            if (info.num_channels == 2U) {
                v = (v + (int16_t)((uint16_t)s[2] | ((uint16_t)s[3] << 8))) / 2;
            }
            block[n] = (int16_t)v;
        }
        o += nf_resample(&rs, block, n, out + o, out_cap - o);
    }
    return o;
}
