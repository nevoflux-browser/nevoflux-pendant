#ifndef NF_PROTO_H
#define NF_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. NFA1 Uplink Audio Frame (24-byte fixed Big-Endian header)
 * ========================================================================= */

#define NFA1_HEADER_SIZE        24U
#define NFA1_FLAG_START         0x01U
#define NFA1_FLAG_END           0x02U
#define NFA1_FLAG_CANCEL        0x04U
#define NFA1_FLAG_VALID_MASK    0x07U

#define NFA1_FORMAT_PCM16_16K   0x01U

typedef struct {
    uint8_t  flags;          /* bit0: START, bit1: END, bit2: CANCEL */
    uint8_t  format;         /* 0x01 = 16kHz / 16-bit signed LE / mono */
    uint16_t reserved;       /* must be 0x0000 */
    uint64_t utterance_id;   /* (boot_nonce_u32 << 32) | (counter_u32++) */
    uint32_t chunk_seq;      /* per-utterance 0, 1, 2... */
    uint32_t frame_seq;      /* per-connection monotonic counter */
} nfa1_header_t;

typedef struct {
    uint32_t boot_nonce;
    uint32_t counter;
} nf_utterance_gen_t;

void nf_utterance_gen_init(nf_utterance_gen_t *gen, uint32_t boot_nonce);
uint64_t nf_utterance_gen_next(nf_utterance_gen_t *gen);

/**
 * Format a uint64_t utterance_id as an unsigned decimal string (e.g. "4294967297").
 * Matches daemon's decimal string utterance_id representation.
 */
int nf_utterance_id_to_str(uint64_t utterance_id, char *out, size_t out_cap);
bool nf_utterance_id_from_str(const char *str, uint64_t *out_id);

bool nfa1_encode_header(const nfa1_header_t *hdr, uint8_t out_buf[NFA1_HEADER_SIZE]);
bool nfa1_decode_header(const uint8_t *buf, size_t len, nfa1_header_t *out_hdr);

/**
 * Pack a complete NFA1 frame (24B header + PCM payload) into out_buf.
 * Returns total bytes written (24 + pcm_len), or 0 on error / insufficient capacity.
 */
size_t nfa1_pack_frame(const nfa1_header_t *hdr,
                       const uint8_t *pcm_bytes,
                       size_t pcm_len,
                       uint8_t *out_buf,
                       size_t out_cap);

/* =========================================================================
 * 2. NFM1 Downlink Media Frame (22 + idLen variable Big-Endian header)
 *    Matches nevoflux-portal/src/lib/chat/media.ts byte-for-byte:
 *    magic(4) "NFM1" | seq(u64 BE) | idLen(u8) | id(idLen) | offset(u64 BE) | flags(u8) | payload
 * ========================================================================= */

#define NFM1_FIXED_HEADER_SIZE  22U
#define NFM1_MAX_ID_LEN         64U
#define NFM1_FLAG_EOF           0x01U
#define NFM1_FLAG_NO_SEQ        0x02U

typedef struct {
    bool          has_seq;
    uint64_t      seq;
    char          id[NFM1_MAX_ID_LEN + 1];
    uint8_t       id_len;
    uint64_t      offset;
    uint8_t       flags;
    bool          eof;
    const uint8_t *payload;
    size_t        payload_len;
} nfm1_frame_t;

bool nfm1_is_media_frame(const uint8_t *buf, size_t len);
bool nfm1_decode_frame(const uint8_t *buf, size_t len, nfm1_frame_t *out_frame);
size_t nfm1_encode_frame(const nfm1_frame_t *frame, uint8_t *out_buf, size_t out_cap);

/* =========================================================================
 * 3. WAV Header Parser (Dynamic Sample Rate for ES8311: 16k / 24k / 48k)
 * ========================================================================= */

#define NF_WAV_MIN_HEADER_SIZE  44U

typedef struct {
    uint32_t sample_rate;      /* e.g. 24000 (Kokoro) or 48000 (MOSS) */
    uint16_t num_channels;     /* 1 (mono) or 2 (stereo) */
    uint16_t bits_per_sample;  /* 16 */
    uint32_t data_size;        /* byte length of PCM data chunk */
    size_t   pcm_offset;       /* byte offset where raw PCM starts (typically 44) */
} nf_wav_info_t;

bool nf_wav_parse_header(const uint8_t *buf, size_t len, nf_wav_info_t *out_info);

/* =========================================================================
 * 4. Relay Plaintext Classifier & Y2 SequenceTracker
 *    Matches nevoflux-portal/src/lib/chat/sequence.ts & relay-logic.ts
 * ========================================================================= */

typedef enum {
    NF_RELAY_TEXT_UNKNOWN = 0,
    NF_RELAY_TEXT_PONG    = 1,
    NF_RELAY_TEXT_PEERS   = 2,
} nf_relay_text_kind_t;

/**
 * Inspect an unencrypted WebSocket TEXT frame from the relay before running
 * AES-GCM decryption. Recognizes "pong" and `{"k":"peers","n":<count>}`.
 */
nf_relay_text_kind_t nf_relay_classify_text(const char *text, size_t len, int *out_peers);

#define NF_SEQ_WINDOW_SIZE      16U
#define NF_SEQ_MAX_FRAME_BYTES  2048U

typedef struct {
    bool     occupied;
    uint64_t seq;
    size_t   len;
    uint8_t  data[NF_SEQ_MAX_FRAME_BYTES];
} nf_seq_slot_t;

typedef struct {
    uint64_t      next_expected;
    nf_seq_slot_t held[NF_SEQ_WINDOW_SIZE];
} nf_seq_tracker_t;

typedef void (*nf_seq_deliver_cb_t)(uint64_t seq, const uint8_t *data, size_t len, void *user_ctx);

void nf_seq_tracker_init(nf_seq_tracker_t *tracker, uint64_t initial_seq);
void nf_seq_tracker_reset(nf_seq_tracker_t *tracker, uint64_t next_seq);
uint64_t nf_seq_tracker_expected(const nf_seq_tracker_t *tracker);

/**
 * Process an inbound sequenced frame on the data channel.
 * Delivers in-order frames (and any contiguous buffered frames) via `cb`.
 * Sets `*out_gap = true` if a sequence gap was detected (caller should send
 * `{"k":"resume","from":expected}`).
 */
bool nf_seq_tracker_receive(nf_seq_tracker_t *tracker,
                            uint64_t seq,
                            const uint8_t *frame_json,
                            size_t frame_len,
                            nf_seq_deliver_cb_t cb,
                            void *user_ctx,
                            bool *out_gap);

int nf_format_resume_msg(uint64_t expected_seq, char *out_buf, size_t out_cap);
int nf_format_asset_pull_msg(const char *asset_id,
                             uint64_t offset,
                             uint32_t length,
                             bool media_channel_open,
                             char *out_buf,
                             size_t out_cap);

/* =========================================================================
 * 5. Channel Crypto: AES-256-GCM (wire = nonce(12B) || ciphertext || tag(16B))
 * ========================================================================= */

#define NF_AES_KEY_SIZE         32U
#define NF_AES_NONCE_SIZE       12U
#define NF_AES_TAG_SIZE         16U
#define NF_AES_OVERHEAD         (NF_AES_NONCE_SIZE + NF_AES_TAG_SIZE)

/**
 * Seal plaintext with AES-256-GCM using a 12-byte nonce.
 * Output layout: nonce(12) || ciphertext(pt_len) || tag(16).
 * Returns total bytes written (pt_len + 28), or 0 on error.
 */
size_t nf_channel_encrypt(const uint8_t key[NF_AES_KEY_SIZE],
                          const uint8_t nonce[NF_AES_NONCE_SIZE],
                          const uint8_t *plaintext,
                          size_t pt_len,
                          uint8_t *out_wire,
                          size_t out_cap);

/**
 * Open a sealed wire frame (nonce(12) || ciphertext || tag(16)).
 * Returns true on authentic decryption and sets `*out_pt_len = wire_len - 28`.
 */
bool nf_channel_decrypt(const uint8_t key[NF_AES_KEY_SIZE],
                        const uint8_t *wire,
                        size_t wire_len,
                        uint8_t *out_plaintext,
                        size_t out_cap,
                        size_t *out_pt_len);

/* =========================================================================
 * 6. Small codecs: base64, JSON string escaping, PCM resampling
 * ========================================================================= */

/** Encoded length without the NUL terminator. */
size_t nf_b64_encoded_len(size_t raw_len);

/** Standard base64 with padding. Returns chars written (excl. NUL), 0 on error. */
size_t nf_b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

/**
 * Decode standard or URL-safe base64 (padding optional). Returns bytes written;
 * `*out_ok` is false on an invalid character.
 */
size_t nf_b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap, bool *out_ok);

/**
 * Walk a `voice_audio` frame's `opus` payload: packets back to back, each after
 * its length as a little-endian u16. Sets `*pkt`/`*pkt_len` to the packet at
 * `*offset` and advances past it. False at the end, or on a length that runs
 * past the buffer (the rest is not trusted).
 */
bool nf_opus_next_packet(const uint8_t *buf, size_t len, size_t *offset, const uint8_t **pkt, size_t *pkt_len);

/** Escape `in` for a JSON string body (no quotes added). Truncates to fit. */
size_t nf_json_escape(const char *in, char *out, size_t out_cap);

typedef struct {
    uint32_t in_rate;
    uint32_t out_rate;
    uint32_t phase;
    int32_t  prev;
    int32_t  x0;
    int32_t  x1;
    bool     primed;
} nf_resampler_t;

/** Mono PCM16 rate converter (e.g. Kokoro's 24 kHz -> the 16 kHz the mic runs at). */
void nf_resampler_init(nf_resampler_t *rs, uint32_t in_rate, uint32_t out_rate);

/** Feed `in_samples`; streaming-safe (state carries across calls). Returns samples written. */
size_t nf_resample(nf_resampler_t *rs,
                   const int16_t *in,
                   size_t in_samples,
                   int16_t *out,
                   size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* NF_PROTO_H */
