#ifndef NF_PLAYER_H
#define NF_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The speaker side of a spoken answer (ESP only): a PSRAM queue of 16 kHz mono
 * PCM drained to the ES8311 in 20 ms blocks. The speaker opens on the first
 * sample and closes (PA off) shortly after the last; the mic keeps running at
 * the same 16 kHz throughout.
 */
bool   nf_player_init(void);
size_t nf_player_push(const int16_t *pcm, size_t samples);
void   nf_player_stop(void);
/** True once each time the queue runs dry after playing. */
bool   nf_player_take_drained(void);

#ifdef __cplusplus
}
#endif

#endif /* NF_PLAYER_H */
