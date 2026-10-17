#include "nevoflux_app.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char   sent_msgs[16][512];
    size_t count;
} tx_capture_t;

static void capture_send_json(const char *wire_json, void *user_ctx)
{
    tx_capture_t *cap = (tx_capture_t *)user_ctx;
    assert(cap->count < 16U);
    snprintf(cap->sent_msgs[cap->count++], sizeof(cap->sent_msgs[0]), "%s", wire_json);
}

static void test_token_mgr_and_relay_url(void)
{
    nf_token_mgr_t tm;
    nf_token_mgr_init(&tm);
    assert(nf_token_mgr_needs_refresh(&tm, 1000ULL));

    char hdrs[256];
    assert(nf_token_mgr_build_request_headers("sess_tok_7d", hdrs, sizeof(hdrs)) > 0);
    assert(strstr(hdrs, "Authorization: Bearer sess_tok_7d") != NULL);
    assert(strstr(hdrs, "User-Agent: nevoflux-pendant/0.1") != NULL);

    /* Mint 15m JWT at t=1000ms */
    assert(nf_token_mgr_parse_response(&tm, 200, "{\"token\":\"jwt_15min_abc\"}", 1000ULL));
    assert(tm.valid && !tm.auth_revoked);
    assert(strcmp(tm.jwt, "jwt_15min_abc") == 0);
    assert(!nf_token_mgr_needs_refresh(&tm, 60000ULL));

    /* Refresh needed within 90s of 15m expiry */
    assert(nf_token_mgr_needs_refresh(&tm, 1000ULL + (14ULL * 60ULL * 1000ULL)));

    /* Build Relay WS URLs matching portal-relay/?c=<id>&t=<jwt> */
    char ws_url[256];
    assert(nf_relay_build_ws_url("wss://relay.nevoflux.app/", "0123456789abcdef0123456789abcdef",
                                 false, tm.jwt, ws_url, sizeof(ws_url)) > 0);
    assert(strcmp(ws_url, "wss://relay.nevoflux.app/?c=0123456789abcdef0123456789abcdef&t=jwt_15min_abc") == 0);

    assert(nf_relay_build_ws_url("wss://relay.nevoflux.app", "0123456789abcdef0123456789abcdef",
                                 true, tm.jwt, ws_url, sizeof(ws_url)) > 0);
    assert(strcmp(ws_url, "wss://relay.nevoflux.app/?c=0123456789abcdef0123456789abcdef-m&t=jwt_15min_abc") == 0);

    /* HTTP 401 marks token revoked and stops infinite retry */
    assert(!nf_token_mgr_parse_response(&tm, 401, "unauthorized", 2000ULL));
    assert(tm.auth_revoked && !nf_token_mgr_needs_refresh(&tm, 3000ULL));
}

static void test_500ms_bounded_uplink_queue(void)
{
    nf_uplink_queue_t q;
    nf_uplink_queue_init(&q);

    uint8_t dummy_frame[32];
    /* Push 30 frames into a 25-frame (500ms) bounded queue */
    for (uint8_t i = 0; i < 30U; ++i) {
        memset(dummy_frame, i, sizeof(dummy_frame));
        assert(nf_uplink_queue_push(&q, dummy_frame, sizeof(dummy_frame)));
    }
    /* Oldest 5 frames (0..4) must be dropped; 25 newest frames (5..29) retained */
    assert(q.count == 25U);
    assert(q.dropped_frames == 5U);

    uint8_t out[32];
    size_t out_len = 0U;
    assert(nf_uplink_queue_pop(&q, out, sizeof(out), &out_len));
    assert(out_len == sizeof(dummy_frame) && out[0] == 5U);
}

static void test_ptt_short_pause_and_20s_auto_split(void)
{
    tx_capture_t cap = {0};
    nf_app_ctx_t app;
    nf_app_init(&app, 0x00000002U, capture_send_json, &cap);

    uint8_t pcm[NF_PCM_BYTES_PER_FRAME];
    memset(pcm, 0x11, sizeof(pcm));

    /* 1. Hold BOOT -> enters LISTENING and enqueues START frame for utterance #0 */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    assert(app.state == NF_APP_STATE_LISTENING);
    assert(app.utterance_open);
    uint64_t first_utt_id = app.current_utterance_id;

    /* 2. Short pause (<20s, e.g. 50 frames = 1s of silence while holding BOOT) MUST NOT split END */
    for (int i = 0; i < 50; ++i) {
        nf_app_on_mic_frame(&app, pcm, false);
    }
    assert(app.utterance_open);
    assert(app.current_utterance_id == first_utt_id);

    /* 3. Speak past 20s (1000 frames total), then hit 1 silence frame -> auto-splits into utterance #1! */
    for (int i = 50; i < 1000; ++i) {
        nf_app_on_mic_frame(&app, pcm, true);
    }
    nf_app_on_mic_frame(&app, pcm, false); /* 1000th frame is silence -> triggers auto-split */
    assert(app.utterance_open);
    assert(app.current_utterance_id == first_utt_id + 1ULL);
    assert(app.pending_finals_count == 2U);

    /* 4. First segment's speech_final arrives while user is STILL holding BOOT -> must NOT send yet! */
    nf_app_on_speech_final(&app, first_utt_id, "帮我查一下上周的", true);
    assert(cap.count == 0U);

    /* 5. User releases BOOT -> enters WAITING_REPLY; second speech_final arrives -> sends concatenated text! */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    assert(app.state == NF_APP_STATE_WAITING_REPLY);
    nf_app_on_speech_final(&app, first_utt_id + 1ULL, "项目会议纪要", true);
    assert(cap.count == 1U);
    assert(strcmp(cap.sent_msgs[0],
                  "{\"k\":\"frame\",\"frame\":{\"kind\":\"send\",\"text\":\"帮我查一下上周的项目会议纪要\"}}") == 0);
}

static void test_meeting_mode_and_barge_in(void)
{
    tx_capture_t cap = {0};
    nf_app_ctx_t app;
    nf_app_init(&app, 0x00000005U, capture_send_json, &cap);

    uint8_t pcm[NF_PCM_BYTES_PER_FRAME];
    memset(pcm, 0x22, sizeof(pcm));

    /* 1. Double-click BOOT -> enters hands-free Meeting Mode */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_DOUBLE_CLICK);
    assert(app.state == NF_APP_STATE_MEETING);
    assert(!app.utterance_open);

    /* Speech onset opens utterance automatically */
    nf_app_on_mic_frame(&app, pcm, true);
    assert(app.utterance_open);
    uint64_t meet_utt = app.current_utterance_id;

    /* 40 frames (800ms) of silence automatically closes utterance with END */
    for (uint32_t i = 0; i < NF_MEETING_SILENCE_FRAMES; ++i) {
        nf_app_on_mic_frame(&app, pcm, false);
    }
    assert(!app.utterance_open);

    /* speech_final in Meeting Mode updates screen transcript WITHOUT sending TTS trigger */
    nf_app_on_speech_final(&app, meet_utt, "第一条会议结论已记录", true);
    assert(strcmp(app.screen_transcript, "第一条会议结论已记录") == 0);
    assert(cap.count == 0U);

    /* Double-click BOOT exits Meeting Mode back to IDLE */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_DOUBLE_CLICK);
    assert(app.state == NF_APP_STATE_IDLE);

    /* 2. Downlink TTS Asset offer -> pipelines 2 x 16KB asset_pull requests */
    nf_app_on_asset_offer(&app, "tts_wav_01", 40000ULL, true);
    assert(app.state == NF_APP_STATE_PLAYING);
    assert(cap.count == 2U);
    assert(strstr(cap.sent_msgs[0], "\"offset\":0,\"length\":16384") != NULL);
    assert(strstr(cap.sent_msgs[1], "\"offset\":16384,\"length\":16384") != NULL);

    /* Simulate first NFM1 chunk arriving with 24kHz WAV header -> switches ES8311 to 24000Hz */
    uint8_t wav24k[64] = {
        'R', 'I', 'F', 'F', 0x24, 0x80, 0x00, 0x00, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0xC0, 0x5D, 0x00, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x02, 0x00, 0x10, 0x00,
        'd', 'a', 't', 'a', 0x00, 0x80, 0x00, 0x00,
        0x01, 0x02, 0x03, 0x04
    };
    nfm1_frame_t frame = {
        .has_seq = false,
        .id = "tts_wav_01",
        .offset = 0ULL,
        .flags = NFM1_FLAG_NO_SEQ,
        .eof = false,
        .payload = wav24k,
        .payload_len = 48U,
    };
    const uint8_t *pcm_ptr = NULL;
    size_t pcm_len = 0U;
    assert(nf_asset_stream_on_nfm1(&app.downlink_stream, &frame, &pcm_ptr, &pcm_len));
    assert(bsp_audio_get_sample_rate() == 24000U);
    assert(pcm_len == 4U && pcm_ptr[0] == 0x01U);

    /* 3. Hold BOOT during PLAYING -> sends voice_barge_in, restores 16kHz, enters LISTENING */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    assert(app.state == NF_APP_STATE_LISTENING);
    assert(bsp_audio_get_sample_rate() == 16000U);
    assert(cap.count == 3U);
    assert(strcmp(cap.sent_msgs[2], "{\"k\":\"frame\",\"frame\":{\"kind\":\"voice_barge_in\"}}") == 0);
}

int main(void)
{
    test_token_mgr_and_relay_url();
    test_500ms_bounded_uplink_queue();
    test_ptt_short_pause_and_20s_auto_split();
    test_meeting_mode_and_barge_in();
    printf("[PASS] All apps/nevoflux integration & FSM tests passed!\n");
    return 0;
}
