#ifndef NEVOFLUX_APP_H
#define NEVOFLUX_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bsp_143c.h"
#include "nf_proto.h"
#include "prov_nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. Two-Level Token Manager (7d account_token -> 15m Relay JWT)
 *    Requires custom User-Agent: nevoflux-pendant/0.1 (Cloudflare 1010 rule)
 * ========================================================================= */

#define NF_HTTP_USER_AGENT          "nevoflux-pendant/0.1"
#define NF_JWT_TTL_MS               (15ULL * 60ULL * 1000ULL)
#define NF_JWT_REFRESH_MARGIN_MS    (90ULL * 1000ULL)

typedef struct {
    char     jwt[512];
    uint64_t minted_at_ms;
    bool     valid;
    bool     auth_revoked; /* true on HTTP 401/403 -> stop retrying, prompt re-pair */
} nf_token_mgr_t;

void nf_token_mgr_init(nf_token_mgr_t *mgr);
bool nf_token_mgr_needs_refresh(const nf_token_mgr_t *mgr, uint64_t now_ms);
int  nf_token_mgr_build_request_headers(const char *account_token, char *out_hdrs, size_t out_cap);
bool nf_token_mgr_parse_response(nf_token_mgr_t *mgr,
                                 int http_status,
                                 const char *json_body,
                                 uint64_t now_ms);

/* =========================================================================
 * 2. Relay WebSocket Manager (data & data-m channels, ?c=<id>&t=<jwt>)
 * ========================================================================= */

#define NF_WS_KEEPALIVE_INTERVAL_MS 30000ULL

int nf_relay_build_ws_url(const char *base_relay_url,
                          const char *channel_id,
                          bool is_media_channel,
                          const char *jwt,
                          char *out_url,
                          size_t out_cap);

/* =========================================================================
 * 3. Bounded 500ms Uplink NFA1 Queue (25 frames @ 20ms = 640B PCM)
 *    Drops oldest frame on overflow while keeping chunk_seq monotonic.
 * ========================================================================= */

#define NF_PCM_FRAME_MS             20U
#define NF_PCM_SAMPLES_PER_FRAME    320U
#define NF_PCM_BYTES_PER_FRAME      640U
#define NF_MAX_NFA1_FRAME_BYTES     (NFA1_HEADER_SIZE + NF_PCM_BYTES_PER_FRAME)
#define NF_UPLINK_QUEUE_CAPACITY    25U /* 25 * 20ms = 500ms max buffered audio */

/* Auto-split long Push-to-Talk utterances at >20s (1000 frames) on VAD silence */
#define NF_PTT_MAX_FRAMES_PER_SEG   1000U
/* Meeting Mode silence threshold: 800ms = 40 frames @ 20ms */
#define NF_MEETING_SILENCE_FRAMES   40U

typedef struct {
    size_t  len;
    uint8_t bytes[NF_MAX_NFA1_FRAME_BYTES];
} nf_uplink_slot_t;

typedef struct {
    nf_uplink_slot_t slots[NF_UPLINK_QUEUE_CAPACITY];
    size_t           head;
    size_t           tail;
    size_t           count;
    uint32_t         dropped_frames;
} nf_uplink_queue_t;

void nf_uplink_queue_init(nf_uplink_queue_t *q);
void nf_uplink_queue_clear(nf_uplink_queue_t *q);
bool nf_uplink_queue_push(nf_uplink_queue_t *q, const uint8_t *frame, size_t len);
bool nf_uplink_queue_pop(nf_uplink_queue_t *q, uint8_t *out_frame, size_t out_cap, size_t *out_len);

/* =========================================================================
 * 4. Downlink Streaming Asset Puller (16KB chunks, MAX_IN_FLIGHT = 2)
 * ========================================================================= */

#define NF_ASSET_PULL_CHUNK_BYTES   16384U
#define NF_ASSET_MAX_IN_FLIGHT      2U
#define NF_DOWNLINK_RB_CAPACITY     65536U /* 64KB PSRAM playback ringbuffer */

typedef struct {
    bool     active;
    char     asset_id[NFM1_MAX_ID_LEN + 1];
    uint64_t total_size;
    uint64_t next_request_offset;
    uint64_t bytes_received;
    uint32_t in_flight;
    bool     eof_reached;
    bool     wav_header_parsed;
    uint32_t sample_rate;
} nf_asset_stream_t;

void nf_asset_stream_init(nf_asset_stream_t *st);
void nf_asset_stream_start(nf_asset_stream_t *st, const char *asset_id, uint64_t total_size);
void nf_asset_stream_stop(nf_asset_stream_t *st);

/**
 * If an `asset_pull` range should be requested right now (`in_flight < 2` and
 * not at EOF), formats the `WireMessage` JSON into `out_json` and increments
 * `in_flight`. Returns true if a request was generated.
 */
bool nf_asset_stream_next_pull(nf_asset_stream_t *st,
                               bool media_channel_open,
                               char *out_json,
                               size_t out_cap);

/**
 * Consume a decoded `NFM1` frame from the media channel.
 * On `offset == 0`, parses the 44-byte WAV header, switches ES8311 sample rate
 * via `bsp_audio_set_play_sample_rate()`, and sets `*out_pcm_ptr` / `*out_pcm_len`
 * to the raw PCM portion (skipping the 44B WAV header).
 */
bool nf_asset_stream_on_nfm1(nf_asset_stream_t *st,
                             const nfm1_frame_t *frame,
                             const uint8_t **out_pcm_ptr,
                             size_t *out_pcm_len);

/* =========================================================================
 * 5. Dual-Mode Application State Machine (PTT vs Meeting Mode)
 * ========================================================================= */

typedef enum {
    NF_APP_STATE_IDLE          = 0,
    NF_APP_STATE_LISTENING     = 1, /* Hold BOOT Push-to-Talk */
    NF_APP_STATE_WAITING_REPLY = 2, /* Released BOOT, waiting for final speech_final */
    NF_APP_STATE_PLAYING       = 3, /* Playing downlink TTS WAV; BOOT triggers barge_in */
    NF_APP_STATE_MEETING       = 4, /* Hands-free continuous VAD recording (/meeting) */
} nf_app_state_t;

typedef void (*nf_send_data_json_cb_t)(const char *wire_json, void *user_ctx);

typedef struct {
    nf_app_state_t         state;
    nf_utterance_gen_t     utt_gen;
    nf_uplink_queue_t      uplink_q;
    nf_asset_stream_t      downlink_stream;

    /* Active utterance tracking */
    bool                   utterance_open;
    uint64_t               current_utterance_id;
    uint32_t               current_chunk_seq;
    uint32_t               conn_frame_seq;
    uint32_t               segment_frames_sent;
    uint32_t               consecutive_silence_frames;
    uint32_t               pending_finals_count;

    /* Concatenated transcript across >20s auto-split PTT segments */
    char                   ptt_accum_text[1024];
    char                   screen_transcript[1024];

    nf_send_data_json_cb_t send_data_json;
    void                  *user_ctx;
} nf_app_ctx_t;

void nf_app_init(nf_app_ctx_t *app,
                 uint32_t boot_nonce,
                 nf_send_data_json_cb_t send_data_json,
                 void *user_ctx);

void nf_app_on_key_event(nf_app_ctx_t *app, bsp_key_event_t ev);

/**
 * Feed one 20ms (640B) 16kHz PCM16 microphone frame along with ESP-SR VAD decision
 * (`is_speech = true` when voice is detected).
 */
void nf_app_on_mic_frame(nf_app_ctx_t *app, const uint8_t pcm_640[NF_PCM_BYTES_PER_FRAME], bool is_speech);

/**
 * Handle `speech_partial` and `speech_final` events from the daemon.
 * In Mode A PTT: concatenates segments and sends `{"k":"frame","frame":{"kind":"send","text":"..."}}`
 * once the user has released BOOT and all segments have finalized.
 * In Meeting Mode: updates `screen_transcript` without sending TTS trigger.
 */
void nf_app_on_speech_partial(nf_app_ctx_t *app, uint64_t utterance_id, const char *text);
void nf_app_on_speech_final(nf_app_ctx_t *app, uint64_t utterance_id, const char *text, bool accepted);

void nf_app_on_asset_offer(nf_app_ctx_t *app, const char *asset_id, uint64_t size, bool media_open);
void nf_app_on_playback_finished(nf_app_ctx_t *app);

#ifdef __cplusplus
}
#endif

#endif /* NEVOFLUX_APP_H */
