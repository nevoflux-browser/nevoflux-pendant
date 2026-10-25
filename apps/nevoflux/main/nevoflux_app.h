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

/*
 * Wire protocol (nevoflux-agent `remote/`): every frame below is a JSON object
 * carried as `{"k":"frame","frame":<it>}`, sealed with AES-256-GCM
 * (nonce(12) || ciphertext || tag(16)) under the channel key, as one binary
 * WebSocket message — over the relay, or straight to the head on the LAN.
 *
 *   up   (data):  speech_start / speech_chunk / speech_end / speech_cancel
 *                 user_message {text, voice:true}   voice_barge_in {turn_id, played}
 *   down (data):  speech_partial / speech_final / speech_error
 *                 stream_delta / turn_end / error
 *                 voice_audio {turn_id, seq, sample_rate, wav:b64} / voice_done / voice_failed
 *   control:      up {kind:"sync"} {kind:"attach",sessionId}; down {kind:"sessions",rows:[...]}
 */

/* =========================================================================
 * 1. Two-Level Token Manager (7d account_token -> 15m Relay JWT)
 *    Requires custom User-Agent: nevoflux-pendant/0.1 (Cloudflare 1010 rule)
 * ========================================================================= */

#define NF_HTTP_USER_AGENT          "nevoflux-pendant/0.1"
#define NF_JWT_TTL_MS               (15ULL * 60ULL * 1000ULL)
#define NF_JWT_REFRESH_MARGIN_MS    (90ULL * 1000ULL)

typedef struct {
    char     jwt[1024];
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
/** The JWT can also come back in the `set-auth-jwt` response header. */
bool nf_token_mgr_accept_header_jwt(nf_token_mgr_t *mgr, const char *jwt, uint64_t now_ms);

/* =========================================================================
 * 2. Channel URLs
 * ========================================================================= */

#define NF_WS_KEEPALIVE_INTERVAL_MS 30000ULL
#define NF_LAN_DEFAULT_PORT         47821U

/** Relay: `<base>/?c=<channel_id>&t=<jwt>`. */
int nf_relay_build_ws_url(const char *base_relay_url,
                          const char *channel_id,
                          bool is_media_channel,
                          const char *jwt,
                          char *out_url,
                          size_t out_cap);

/** LAN: `ws://<ip>:<port>/?c=<channel_id>` (no token: the channel key is the credential). */
int nf_lan_build_ws_url(const char *ip, uint16_t port, const char *channel_id, char *out_url, size_t out_cap);

/** First 8 hex digits of SHA-256(channel_id): what the head advertises in mDNS TXT `ch`. */
void nf_lan_channel_tag(const char *channel_id, char out_tag[9]);

/** True if the comma-separated mDNS TXT `ch` value lists `tag`. */
bool nf_lan_txt_has_tag(const char *txt_ch, const char *tag);

/** Wrap a frame object as the uplink `WireMessage` (`{"k":"frame","frame":...}`). */
int nf_wire_wrap_frame(const char *frame_json, char *out, size_t out_cap);

/* =========================================================================
 * 3. Speech uplink framing
 * ========================================================================= */

#define NF_PCM_FRAME_MS             20U
#define NF_PCM_SAMPLES_PER_FRAME    320U
#define NF_PCM_BYTES_PER_FRAME      640U
/* One speech_chunk per 200 ms: 5 messages/s instead of 50 (the relay bills per message) */
#define NF_CHUNK_FRAMES             10U
#define NF_CHUNK_PCM_BYTES          (NF_CHUNK_FRAMES * NF_PCM_BYTES_PER_FRAME)
#define NF_CHUNK_JSON_CAP           (NF_CHUNK_PCM_BYTES * 4U / 3U + 160U)
/* Shorter than this at release is a tap, not speech */
#define NF_MIN_UTTERANCE_FRAMES     15U
/* Auto-split long Push-to-Talk utterances at >20s (1000 frames) on VAD silence */
#define NF_PTT_MAX_FRAMES_PER_SEG   1000U
/* Meeting Mode silence threshold: 800ms = 40 frames @ 20ms */
#define NF_MEETING_SILENCE_FRAMES   40U
/* turn_end with no voice_audio yet: wait this long for speech before giving up */
#define NF_VOICE_GRACE_MS           8000ULL
#define NF_MAX_PENDING_FINALS       8U
#define NF_ID_CAP                   48U

/* =========================================================================
 * 4. Application State Machine (PTT vs Meeting Mode)
 * ========================================================================= */

typedef enum {
    NF_APP_STATE_IDLE          = 0,
    NF_APP_STATE_LISTENING     = 1, /* Hold BOOT Push-to-Talk */
    NF_APP_STATE_WAITING_REPLY = 2, /* Released BOOT: transcribing, then thinking */
    NF_APP_STATE_PLAYING       = 3, /* Playing the spoken answer; BOOT barges in */
    NF_APP_STATE_MEETING       = 4, /* Hands-free continuous VAD recording */
} nf_app_state_t;

typedef struct {
    /* One frame object (not yet wrapped/sealed) for the data channel */
    void (*send_frame)(const char *frame_json, void *ctx);
    /* Drop everything queued for the speaker, right now (barge-in / cancel) */
    void (*stop_playback)(void *ctx);
    void *ctx;
} nf_app_io_t;

typedef struct {
    nf_app_state_t     state;
    nf_app_io_t        io;
    nf_utterance_gen_t utt_gen;
    bool               link_up;

    /* Active utterance */
    bool     utterance_open;
    char     utterance_id[NF_ID_CAP];
    uint32_t chunk_seq;
    uint32_t segment_frames;
    uint32_t press_frames;
    uint32_t consecutive_silence_frames;
    uint8_t  chunk_pcm[NF_CHUNK_PCM_BYTES];
    size_t   chunk_len;

    /* Utterances of this press, in order, until every speech_final is in */
    struct {
        char id[NF_ID_CAP];
        bool done;
        char text[512];
    } pending[NF_MAX_PENDING_FINALS];
    size_t   pending_count;

    bool     submitted;               /* this press's user_message went out */

    /* The answer */
    char     turn_id[NF_ID_CAP];      /* voice turn being played ("" = none yet) */
    char     barged_turn_id[NF_ID_CAP];
    uint32_t sentences_queued;
    bool     voice_done;
    bool     turn_ended;
    uint64_t turn_end_ms;
    uint64_t now_ms;

    char ptt_accum_text[1024];
    char screen_transcript[1024];
    char json_scratch[NF_CHUNK_JSON_CAP];
} nf_app_ctx_t;

void nf_app_init(nf_app_ctx_t *app, uint32_t boot_nonce, const nf_app_io_t *io);
void nf_app_set_link(nf_app_ctx_t *app, bool up);
void nf_app_tick(nf_app_ctx_t *app, uint64_t now_ms);

void nf_app_on_key_event(nf_app_ctx_t *app, bsp_key_event_t ev);

/** One 20ms (640B) 16kHz PCM16 mic frame plus the VAD decision. */
void nf_app_on_mic_frame(nf_app_ctx_t *app, const uint8_t pcm_640[NF_PCM_BYTES_PER_FRAME], bool is_speech);

void nf_app_on_speech_partial(nf_app_ctx_t *app, const char *utterance_id, const char *text);
void nf_app_on_speech_final(nf_app_ctx_t *app, const char *utterance_id, const char *text, bool accepted);
void nf_app_on_speech_error(nf_app_ctx_t *app, const char *utterance_id, const char *message);
void nf_app_on_stream_delta(nf_app_ctx_t *app, const char *delta);
void nf_app_on_turn_end(nf_app_ctx_t *app);
void nf_app_on_error(nf_app_ctx_t *app, const char *message);

/**
 * A sentence of spoken answer arrived. Returns true if it belongs to the turn
 * being answered and should be played; false for a stale or barged-in turn.
 */
bool nf_app_on_voice_audio(nf_app_ctx_t *app, const char *turn_id, uint32_t seq);
void nf_app_on_voice_done(nf_app_ctx_t *app, const char *turn_id);
void nf_app_on_voice_failed(nf_app_ctx_t *app, const char *turn_id, const char *message);

/** The speaker ran out of queued audio. */
void nf_app_on_playback_drained(nf_app_ctx_t *app);

/* =========================================================================
 * 5. Spoken answer: WAV (base64-decoded) -> 16 kHz mono PCM
 * ========================================================================= */

/**
 * Convert one `voice_audio` WAV to 16 kHz mono PCM16. Returns samples written,
 * 0 if the WAV is not 16-bit PCM. Stereo is down-mixed.
 */
size_t nf_reply_wav_to_16k(const uint8_t *wav, size_t wav_len, int16_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* NEVOFLUX_APP_H */
