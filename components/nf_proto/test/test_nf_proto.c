#include "nf_proto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_utterance_id_generation_and_str(void)
{
    nf_utterance_gen_t gen;
    nf_utterance_gen_init(&gen, 0x00000001U);

    uint64_t id0 = nf_utterance_gen_next(&gen);
    uint64_t id1 = nf_utterance_gen_next(&gen);
    assert(id0 == 4294967296ULL);
    assert(id1 == 4294967297ULL);

    char str_buf[32];
    int len = nf_utterance_id_to_str(id1, str_buf, sizeof(str_buf));
    assert(len > 0);
    assert(strcmp(str_buf, "4294967297") == 0);

    uint64_t parsed = 0ULL;
    assert(nf_utterance_id_from_str(str_buf, &parsed));
    assert(parsed == id1);

    /* Reject malformed decimal strings */
    assert(!nf_utterance_id_from_str("", &parsed));
    assert(!nf_utterance_id_from_str("429496a", &parsed));
}

static void test_nfa1_encode_decode_vectors(void)
{
    nfa1_header_t hdr = {
        .flags = NFA1_FLAG_START,
        .format = NFA1_FORMAT_PCM16_16K,
        .reserved = 0U,
        .utterance_id = 0x0102030405060708ULL,
        .chunk_seq = 0U,
        .frame_seq = 42U,
    };

    uint8_t raw[NFA1_HEADER_SIZE];
    assert(nfa1_encode_header(&hdr, raw));

    /* Exact byte-level verification of Big-Endian 24B layout */
    const uint8_t expected[24] = {
        'N', 'F', 'A', '1',
        0x01,                   /* flags: START */
        0x01,                   /* format: 16kHz PCM16 LE mono */
        0x00, 0x00,             /* reserved */
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, /* utterance_id BE */
        0x00, 0x00, 0x00, 0x00, /* chunk_seq BE */
        0x00, 0x00, 0x00, 0x2A  /* frame_seq BE = 42 */
    };
    assert(memcmp(raw, expected, sizeof(expected)) == 0);

    nfa1_header_t dec;
    assert(nfa1_decode_header(raw, sizeof(raw), &dec));
    assert(dec.flags == hdr.flags);
    assert(dec.format == hdr.format);
    assert(dec.reserved == 0U);
    assert(dec.utterance_id == hdr.utterance_id);
    assert(dec.chunk_seq == hdr.chunk_seq);
    assert(dec.frame_seq == hdr.frame_seq);

    /* Negative vectors: wrong magic, bad flags, bad format, non-zero reserved, truncated */
    uint8_t bad[24];
    memcpy(bad, expected, 24);
    bad[3] = '2';
    assert(!nfa1_decode_header(bad, 24, &dec));

    memcpy(bad, expected, 24);
    bad[4] = 0x80U; /* invalid flag bit */
    assert(!nfa1_decode_header(bad, 24, &dec));

    memcpy(bad, expected, 24);
    bad[5] = 0x02U; /* unsupported format */
    assert(!nfa1_decode_header(bad, 24, &dec));

    memcpy(bad, expected, 24);
    bad[7] = 0x01U; /* non-zero reserved */
    assert(!nfa1_decode_header(bad, 24, &dec));

    assert(!nfa1_decode_header(expected, 23, &dec));

    /* Pack frame with 640B (20ms) PCM and pure CANCEL empty frame */
    uint8_t pcm[640];
    memset(pcm, 0x5A, sizeof(pcm));
    uint8_t frame_buf[700];
    size_t packed_len = nfa1_pack_frame(&hdr, pcm, sizeof(pcm), frame_buf, sizeof(frame_buf));
    assert(packed_len == 24U + 640U);
    assert(memcmp(frame_buf + 24, pcm, sizeof(pcm)) == 0);

    hdr.flags = NFA1_FLAG_CANCEL;
    assert(nfa1_pack_frame(&hdr, pcm, sizeof(pcm), frame_buf, sizeof(frame_buf)) == 0U);
    assert(nfa1_pack_frame(&hdr, NULL, 0U, frame_buf, sizeof(frame_buf)) == 24U);
}

static void test_nfm1_encode_decode(void)
{
    const uint8_t sample_payload[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    nfm1_frame_t src = {
        .has_seq = false,
        .seq = 0ULL,
        .id = "tts_turn_01-part_0",
        .offset = 16384ULL,
        .flags = NFM1_FLAG_EOF | NFM1_FLAG_NO_SEQ,
        .eof = true,
        .payload = sample_payload,
        .payload_len = sizeof(sample_payload),
    };

    uint8_t wire[128];
    size_t n = nfm1_encode_frame(&src, wire, sizeof(wire));
    assert(n == 22U + strlen(src.id) + sizeof(sample_payload));
    assert(nfm1_is_media_frame(wire, n));

    nfm1_frame_t dst;
    assert(nfm1_decode_frame(wire, n, &dst));
    assert(!dst.has_seq);
    assert(strcmp(dst.id, "tts_turn_01-part_0") == 0);
    assert(dst.offset == 16384ULL);
    assert(dst.eof == true);
    assert(dst.payload_len == sizeof(sample_payload));
    assert(memcmp(dst.payload, sample_payload, sizeof(sample_payload)) == 0);

    /* Reject path traversal in asset id (matches nevoflux-portal SAFE_ID) */
    nfm1_frame_t unsafe = src;
    strcpy(unsafe.id, "../secret");
    assert(nfm1_encode_frame(&unsafe, wire, sizeof(wire)) == 0U);

    /* Reject truncated buffer claiming longer idLen */
    wire[12] = 60U;
    assert(!nfm1_decode_frame(wire, n, &dst));
}

static void test_wav_header_parser(void)
{
    /* Standard 44-byte WAV header for 24kHz mono 16-bit PCM (Kokoro v1.1-zh output) */
    uint8_t wav24k[48] = {
        'R', 'I', 'F', 'F',
        0x24, 0x80, 0x00, 0x00, /* file size - 8 */
        'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ',
        0x10, 0x00, 0x00, 0x00, /* fmt chunk size = 16 */
        0x01, 0x00,             /* audio format = 1 (PCM) */
        0x01, 0x00,             /* num channels = 1 */
        0xC0, 0x5D, 0x00, 0x00, /* sample rate = 24000 (0x00005DC0 LE) */
        0x80, 0xBB, 0x00, 0x00, /* byte rate = 48000 */
        0x02, 0x00,             /* block align = 2 */
        0x10, 0x00,             /* bits per sample = 16 */
        'd', 'a', 't', 'a',
        0x00, 0x80, 0x00, 0x00, /* data size = 32768 */
        0xAA, 0xBB, 0xCC, 0xDD  /* first 4 bytes of PCM */
    };

    nf_wav_info_t info;
    assert(nf_wav_parse_header(wav24k, sizeof(wav24k), &info));
    assert(info.sample_rate == 24000U);
    assert(info.num_channels == 1U);
    assert(info.bits_per_sample == 16U);
    assert(info.data_size == 32768U);
    assert(info.pcm_offset == 44U);

    /* Modify sample rate to 48000Hz (0x0000BB80 LE, MOSS TTS) */
    wav24k[24] = 0x80U;
    wav24k[25] = 0xBBU;
    wav24k[26] = 0x00U;
    wav24k[27] = 0x00U;
    assert(nf_wav_parse_header(wav24k, sizeof(wav24k), &info));
    assert(info.sample_rate == 48000U);
}

typedef struct {
    uint64_t delivered_seqs[8];
    size_t   count;
} seq_test_ctx_t;

static void on_seq_deliver(uint64_t seq, const uint8_t *data, size_t len, void *user_ctx)
{
    (void)data;
    (void)len;
    seq_test_ctx_t *ctx = (seq_test_ctx_t *)user_ctx;
    ctx->delivered_seqs[ctx->count++] = seq;
}

static void test_sequence_tracker_and_relay_text(void)
{
    int peers = -1;
    assert(nf_relay_classify_text("pong", 4, &peers) == NF_RELAY_TEXT_PONG);
    assert(nf_relay_classify_text("{\"k\":\"peers\",\"n\":1}", 19, &peers) == NF_RELAY_TEXT_PEERS);
    assert(peers == 1);
    assert(nf_relay_classify_text("{\"k\":\"peers\",\"n\":0}", 19, &peers) == NF_RELAY_TEXT_PEERS);
    assert(peers == 0);

    nf_seq_tracker_t tracker;
    nf_seq_tracker_init(&tracker, 0ULL);
    seq_test_ctx_t ctx = {0};
    bool gap = false;

    const uint8_t dummy[] = "{\"kind\":\"speech_partial\"}";
    /* Frame 0 arrives in order */
    assert(nf_seq_tracker_receive(&tracker, 0ULL, dummy, sizeof(dummy) - 1, on_seq_deliver, &ctx, &gap));
    assert(!gap && ctx.count == 1 && ctx.delivered_seqs[0] == 0ULL);

    /* Frame 2 arrives out-of-order (gap at 1) */
    assert(nf_seq_tracker_receive(&tracker, 2ULL, dummy, sizeof(dummy) - 1, on_seq_deliver, &ctx, &gap));
    assert(gap && ctx.count == 1 && nf_seq_tracker_expected(&tracker) == 1ULL);

    /* Duplicate frame 0 is ignored without gap */
    assert(nf_seq_tracker_receive(&tracker, 0ULL, dummy, sizeof(dummy) - 1, on_seq_deliver, &ctx, &gap));
    assert(!gap && ctx.count == 1);

    /* Frame 1 arrives -> delivers both 1 and held 2! */
    assert(nf_seq_tracker_receive(&tracker, 1ULL, dummy, sizeof(dummy) - 1, on_seq_deliver, &ctx, &gap));
    assert(!gap && ctx.count == 3);
    assert(ctx.delivered_seqs[1] == 1ULL && ctx.delivered_seqs[2] == 2ULL);
    assert(nf_seq_tracker_expected(&tracker) == 3ULL);

    char msg[160];
    assert(nf_format_resume_msg(3ULL, msg, sizeof(msg)) > 0);
    assert(strcmp(msg, "{\"k\":\"resume\",\"from\":3}") == 0);

    assert(nf_format_asset_pull_msg("asset_1", 16384ULL, 16384U, true, msg, sizeof(msg)) > 0);
    assert(strstr(msg, "\"kind\":\"asset_pull\"") != NULL);
    assert(strstr(msg, "\"mediaChannel\":true") != NULL);
}

static void test_aes_256_gcm_channel_crypto(void)
{
    uint8_t key[NF_AES_KEY_SIZE];
    uint8_t nonce[NF_AES_NONCE_SIZE];
    for (size_t i = 0; i < sizeof(key); ++i) {
        key[i] = (uint8_t)(i * 3U + 1U);
    }
    for (size_t i = 0; i < sizeof(nonce); ++i) {
        nonce[i] = (uint8_t)(0xA0U + i);
    }

    const char *plaintext = "{\"k\":\"frame\",\"frame\":{\"kind\":\"send\",\"text\":\"你好 NevoFlux\"}}";
    size_t pt_len = strlen(plaintext);

    uint8_t wire[256];
    size_t wire_len = nf_channel_encrypt(key, nonce, (const uint8_t *)plaintext, pt_len, wire, sizeof(wire));
    assert(wire_len == pt_len + NF_AES_OVERHEAD);
    assert(memcmp(wire, nonce, NF_AES_NONCE_SIZE) == 0);

    uint8_t decrypted[256];
    size_t dec_len = 0U;
    assert(nf_channel_decrypt(key, wire, wire_len, decrypted, sizeof(decrypted), &dec_len));
    assert(dec_len == pt_len);
    assert(memcmp(decrypted, plaintext, pt_len) == 0);

    /* Tamper with 1 bit of ciphertext -> authentication tag must fail */
    wire[NF_AES_NONCE_SIZE + 2] ^= 0x01U;
    assert(!nf_channel_decrypt(key, wire, wire_len, decrypted, sizeof(decrypted), &dec_len));
}

int main(void)
{
    test_utterance_id_generation_and_str();
    test_nfa1_encode_decode_vectors();
    test_nfm1_encode_decode();
    test_wav_header_parser();
    test_sequence_tracker_and_relay_text();
    test_aes_256_gcm_channel_crypto();
    printf("[PASS] All nf_proto unit tests passed!\n");
    return 0;
}
