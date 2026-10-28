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
 * sample and switches the PA off shortly after the last; the mic keeps running at
 * the same 16 kHz throughout.
 */
bool   nf_player_init(void);
size_t nf_player_push(const int16_t *pcm, size_t samples);
/**
 * Queue one spoken sentence as length-prefixed 16 kHz mono Opus (u16 LE length +
 * packet, repeated). Takes ownership of `framed` (heap). It is decoded as the
 * speaker needs it, so an answer of any length can arrive ahead of playback.
 */
bool   nf_player_queue_opus(uint8_t *framed, size_t len);
void   nf_player_stop(void);
/** True once each time the queue (PCM and queued sentences) runs dry after playing. */
bool   nf_player_take_drained(void);
/** 0..100: how loud the block now playing is (0 once the speaker is idle). */
uint8_t nf_player_level(void);

#ifdef __cplusplus
}
#endif

#endif /* NF_PLAYER_H */
