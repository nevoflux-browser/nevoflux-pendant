#include "nevoflux_app.h"
#include "nf_addr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char   sent[256][9000];
    size_t count;
    int    stops;
} tx_capture_t;

static tx_capture_t s_cap;

static void capture_frame(const char *frame_json, void *ctx)
{
    tx_capture_t *cap = (tx_capture_t *)ctx;
    assert(cap->count < 256U);
    snprintf(cap->sent[cap->count++], sizeof(cap->sent[0]), "%s", frame_json);
}

static void capture_stop(void *ctx)
{
    ((tx_capture_t *)ctx)->stops++;
}

static size_t count_kind(const char *kind)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"kind\":\"%s\"", kind);
    size_t n = 0;
    for (size_t i = 0; i < s_cap.count; ++i) {
        if (strstr(s_cap.sent[i], needle)) {
            n++;
        }
    }
    return n;
}

static const char *last_kind(const char *kind)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"kind\":\"%s\"", kind);
    for (size_t i = s_cap.count; i > 0; --i) {
        if (strstr(s_cap.sent[i - 1U], needle)) {
            return s_cap.sent[i - 1U];
        }
    }
    return NULL;
}

static void new_app(nf_app_ctx_t *app)
{
    memset(&s_cap, 0, sizeof(s_cap));
    nf_app_io_t io = {.send_frame = capture_frame, .stop_playback = capture_stop, .ctx = &s_cap};
    nf_app_init(app, 0x00000001U, &io);
    nf_app_set_link(app, true);
}

static void feed(nf_app_ctx_t *app, size_t frames, bool speech)
{
    uint8_t pcm[NF_PCM_BYTES_PER_FRAME];
    memset(pcm, 0x11, sizeof(pcm));
    for (size_t i = 0; i < frames; ++i) {
        nf_app_on_mic_frame(app, pcm, speech);
    }
}

static void test_token_mgr_and_urls(void)
{
    nf_token_mgr_t tm;
    nf_token_mgr_init(&tm);
    assert(nf_token_mgr_needs_refresh(&tm, 1000ULL));

    char hdrs[256];
    assert(nf_token_mgr_build_request_headers("sess_tok_7d", hdrs, sizeof(hdrs)) > 0);
    assert(strstr(hdrs, "Authorization: Bearer sess_tok_7d") != NULL);
    assert(strstr(hdrs, "User-Agent: nevoflux-pendant/0.1") != NULL);

    assert(nf_token_mgr_parse_response(&tm, 200, "{\"token\":\"jwt_15min_abc\"}", 1000ULL));
    assert(tm.valid && strcmp(tm.jwt, "jwt_15min_abc") == 0);
    assert(!nf_token_mgr_needs_refresh(&tm, 60000ULL));
    assert(nf_token_mgr_needs_refresh(&tm, 1000ULL + (14ULL * 60ULL * 1000ULL)));
    assert(nf_token_mgr_accept_header_jwt(&tm, "hdr.jwt.sig", 5000ULL));
    assert(strcmp(tm.jwt, "hdr.jwt.sig") == 0);

    char url[256];
    assert(nf_relay_build_ws_url("wss://relay.nevoflux.app/", "0123456789abcdef0123456789abcdef",
                                 false, tm.jwt, url, sizeof(url)) > 0);
    assert(strcmp(url, "wss://relay.nevoflux.app/?c=0123456789abcdef0123456789abcdef&t=hdr.jwt.sig") == 0);
    assert(nf_lan_build_ws_url("192.168.1.5", 0, "abcd", url, sizeof(url)) > 0);
    assert(strcmp(url, "ws://192.168.1.5:47821/?c=abcd") == 0);

    /* Must match nevoflux-agent remote/lan.rs channel_tag("abc") */
    char tag[9];
    nf_lan_channel_tag("abc", tag);
    assert(strcmp(tag, "ba7816bf") == 0);
    assert(nf_lan_txt_has_tag("00000000,ba7816bf", tag));
    assert(nf_lan_txt_has_tag("ba7816bf", tag));
    assert(!nf_lan_txt_has_tag("ba7816bf0,1", tag));
    assert(!nf_lan_txt_has_tag("", tag));

    char wire[128];
    assert(nf_wire_wrap_frame("{\"kind\":\"sync\"}", wire, sizeof(wire)) > 0);
    assert(strcmp(wire, "{\"k\":\"frame\",\"frame\":{\"kind\":\"sync\"}}") == 0);

    assert(!nf_token_mgr_parse_response(&tm, 401, "unauthorized", 2000ULL));
    assert(tm.auth_revoked && !nf_token_mgr_needs_refresh(&tm, 3000ULL));
    printf("[PASS] token manager, relay/LAN urls, mDNS tag, wire wrap\n");
}

static void test_ptt_chunks_and_turn(void)
{
    static nf_app_ctx_t app;
    new_app(&app);

    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    assert(app.state == NF_APP_STATE_LISTENING);
    assert(count_kind("speech_start") == 1U);
    assert(strstr(last_kind("speech_start"), "\"utterance_id\":\"4294967296\""));
    assert(strstr(last_kind("speech_start"), "\"sample_rate\":16000"));

    /* 1 s of speech = 50 frames = five 200 ms chunks */
    feed(&app, 50, true);
    assert(count_kind("speech_chunk") == 5U);
    const char *chunk = last_kind("speech_chunk");
    assert(strstr(chunk, "\"seq\":4"));
    /* 6400 PCM bytes -> 8536 base64 chars */
    const char *pcm = strstr(chunk, "\"pcm\":\"") + 7;
    assert(strlen(pcm) == 8536U + 2U); /* + closing quote and brace */

    feed(&app, 3, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    assert(app.state == NF_APP_STATE_WAITING_REPLY);
    assert(count_kind("speech_chunk") == 6U); /* the tail was flushed */
    assert(count_kind("speech_end") == 1U);

    nf_app_on_speech_partial(&app, "4294967296", "今天");
    nf_app_on_speech_final(&app, "999", "stale", true); /* not ours: ignored */
    assert(count_kind("user_message") == 0U);
    nf_app_on_speech_final(&app, "4294967296", "今天天气\"好\"", true);
    assert(count_kind("user_message") == 1U);
    const char *um = last_kind("user_message");
    assert(strstr(um, "\"text\":\"今天天气\\\"好\\\"\""));
    assert(strstr(um, "\"voice\":true"));
    assert(strstr(um, "\"voice_codec\":\"opus\""));

    /* The answer: text streams in, then sentences are spoken */
    nf_app_on_stream_delta(&app, "晴天。");
    assert(strstr(app.screen_transcript, "晴天"));
    assert(nf_app_on_voice_audio(&app, "vt_1", 0));
    assert(app.state == NF_APP_STATE_PLAYING);
    assert(!nf_app_on_voice_audio(&app, "vt_other", 0));
    assert(nf_app_on_voice_audio(&app, "vt_1", 1));
    nf_app_on_turn_end(&app);
    nf_app_on_playback_drained(&app); /* voice not done yet: keep playing */
    assert(app.state == NF_APP_STATE_PLAYING);
    nf_app_on_voice_done(&app, "vt_1");
    nf_app_on_playback_drained(&app);
    assert(app.state == NF_APP_STATE_IDLE);
    printf("[PASS] push-to-talk: 200ms chunks, final -> user_message(voice), spoken answer\n");
}

/* Stands in for Opus: a 3-byte "packet" per frame, numbered so order shows */
static int s_enc_resets;
static uint8_t s_enc_next;

static size_t fake_encode(const int16_t *pcm, uint8_t *out, size_t cap, void *ctx)
{
    (void)pcm;
    (void)ctx;
    assert(cap >= 3U);
    out[0] = 0xF8;
    out[1] = s_enc_next++;
    out[2] = 0x00;
    return 3U;
}

static void fake_encode_reset(void *ctx)
{
    (void)ctx;
    s_enc_resets++;
    s_enc_next = 0U;
}

static void test_opus_uplink(void)
{
    static nf_app_ctx_t app;
    memset(&s_cap, 0, sizeof(s_cap));
    s_enc_resets = 0;
    nf_app_io_t io = {.send_frame = capture_frame,
                      .stop_playback = capture_stop,
                      .encode_frame = fake_encode,
                      .encode_reset = fake_encode_reset,
                      .ctx = &s_cap};
    nf_app_init(&app, 0x00000001U, &io);
    nf_app_set_link(&app, true);

    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    assert(s_enc_resets == 1);
    assert(strstr(last_kind("speech_start"), "\"codec\":\"opus\",\"frame_ms\":20"));

    /* Still one chunk per 10 frames, each frame a u16 LE length + its packet */
    feed(&app, 25, true);
    assert(count_kind("speech_chunk") == 2U);
    const char *chunk = last_kind("speech_chunk");
    assert(strstr(chunk, "\"seq\":1"));
    assert(!strstr(chunk, "\"pcm\""));
    const char *b64 = strstr(chunk, "\"opus\":\"") + 8;
    uint8_t raw[64];
    bool ok = false;
    size_t n = nf_b64_decode(b64, strlen(b64) - 2U, raw, sizeof(raw), &ok);
    assert(ok);
    assert(n == 10U * 5U);
    size_t off = 0U;
    const uint8_t *pkt = NULL;
    size_t pkt_len = 0U;
    for (uint8_t i = 0; i < 10U; ++i) {
        assert(nf_opus_next_packet(raw, n, &off, &pkt, &pkt_len));
        assert(pkt_len == 3U && pkt[0] == 0xF8 && pkt[1] == 10U + i);
    }
    assert(!nf_opus_next_packet(raw, n, &off, &pkt, &pkt_len));

    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    assert(count_kind("speech_chunk") == 3U); /* the 5-frame tail */
    printf("[PASS] Opus uplink: codec announced, encoder reset per utterance, packets framed\n");
}

static void test_tap_is_cancelled_and_silence_is_told(void)
{
    static nf_app_ctx_t app;
    new_app(&app);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 5, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    assert(app.state == NF_APP_STATE_IDLE);
    assert(count_kind("speech_cancel") == 1U);
    assert(strstr(last_kind("speech_cancel"), "\"reason\":\"too_short\""));

    /* A real press whose transcript is rejected: no turn, back to idle */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 30, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_on_speech_final(&app, app.utterance_id, "", false);
    assert(count_kind("user_message") == 0U);
    assert(app.state == NF_APP_STATE_IDLE);

    /* Link down: pressing does nothing on the wire */
    nf_app_set_link(&app, false);
    size_t before = s_cap.count;
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    assert(s_cap.count == before && app.state == NF_APP_STATE_IDLE);
    printf("[PASS] tap -> speech_cancel(too_short); rejected final -> idle; link down -> no-op\n");
}

static void test_20s_split_joins_transcripts(void)
{
    static nf_app_ctx_t app;
    new_app(&app);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    char first[NF_ID_CAP];
    snprintf(first, sizeof(first), "%s", app.utterance_id);
    feed(&app, NF_PTT_MAX_FRAMES_PER_SEG, true);
    assert(count_kind("speech_end") == 0U); /* still speaking: no split */
    feed(&app, 1, false);                   /* first silence past 20 s: split */
    assert(count_kind("speech_end") == 1U);
    assert(count_kind("speech_start") == 2U);
    char second[NF_ID_CAP];
    snprintf(second, sizeof(second), "%s", app.utterance_id);
    assert(strcmp(first, second) != 0);
    feed(&app, 20, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);

    nf_app_on_speech_final(&app, second, "第二段", true); /* out of order */
    assert(count_kind("user_message") == 0U);
    nf_app_on_speech_final(&app, first, "第一段", true);
    assert(count_kind("user_message") == 1U);
    assert(strstr(last_kind("user_message"), "\"text\":\"第一段第二段\""));
    printf("[PASS] >20s push-to-talk splits on silence, one turn after the last final\n");
}

static void test_barge_in_and_meeting(void)
{
    static nf_app_ctx_t app;
    new_app(&app);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 20, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_on_speech_final(&app, app.pending[0].id, "讲个故事", true);
    assert(nf_app_on_voice_audio(&app, "vt_9", 0));
    assert(nf_app_on_voice_audio(&app, "vt_9", 1));

    /* Pressing BOOT mid-answer: local stop first, then voice_barge_in, then a new utterance */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    assert(s_cap.stops == 1);
    const char *bi = last_kind("voice_barge_in");
    assert(bi && strstr(bi, "\"turn_id\":\"vt_9\"") && strstr(bi, "\"played\":1"));
    assert(app.state == NF_APP_STATE_LISTENING);
    assert(!nf_app_on_voice_audio(&app, "vt_9", 2)); /* late audio of the barged turn */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_PWR_CLICK);
    assert(app.state == NF_APP_STATE_IDLE);
    assert(strstr(last_kind("speech_cancel"), "user_stopped"));

    /* Meeting: VAD-driven segments, transcripts on screen, never a turn */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_DOUBLE_CLICK);
    assert(app.state == NF_APP_STATE_MEETING);
    size_t starts = count_kind("speech_start");
    feed(&app, 10, false);
    assert(count_kind("speech_start") == starts);
    feed(&app, 30, true);
    feed(&app, NF_MEETING_SILENCE_FRAMES, false);
    assert(count_kind("speech_start") == starts + 1U);
    assert(count_kind("speech_end") >= 1U);
    char seg1[NF_ID_CAP];
    snprintf(seg1, sizeof(seg1), "%s", app.utterance_id);
    feed(&app, 30, true);
    feed(&app, NF_MEETING_SILENCE_FRAMES, false);
    nf_app_on_speech_final(&app, app.utterance_id, "会议第二句", true); /* out of order */
    nf_app_on_speech_final(&app, seg1, "会议第一句", true);
    assert(strstr(app.screen_transcript, "会议第一句"));
    assert(strstr(app.screen_transcript, "会议第二句"));
    size_t turns = count_kind("user_message");
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_DOUBLE_CLICK);
    assert(app.state == NF_APP_STATE_IDLE && count_kind("user_message") == turns);
    printf("[PASS] barge-in (stop, voice_barge_in, ignore late audio), meeting mode\n");
}

static void test_text_only_answer_times_out(void)
{
    static nf_app_ctx_t app;
    new_app(&app);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 20, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_on_speech_final(&app, app.pending[0].id, "你好", true);
    nf_app_tick(&app, 1000ULL);
    nf_app_on_turn_end(&app);
    nf_app_tick(&app, 1000ULL + NF_VOICE_GRACE_MS - 1ULL);
    assert(app.state == NF_APP_STATE_WAITING_REPLY);
    nf_app_tick(&app, 1000ULL + NF_VOICE_GRACE_MS);
    assert(app.state == NF_APP_STATE_IDLE);
    assert(strstr(app.screen_prompt, "没有语音回复"));
    /* ...and if its voice turns up after all, it still plays */
    assert(nf_app_on_voice_audio(&app, "vt_late", 0));
    assert(app.state == NF_APP_STATE_PLAYING);

    /* More text after a stream_end keeps it waiting (agent mode: text, tool, text) */
    new_app(&app);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 20, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_on_speech_final(&app, app.pending[0].id, "查一下", true);
    nf_app_tick(&app, 1000ULL);
    nf_app_on_turn_end(&app);
    nf_app_tick(&app, 5000ULL);
    nf_app_on_stream_delta(&app, "找到了");
    nf_app_tick(&app, 1000ULL + NF_VOICE_GRACE_MS + 10ULL);
    assert(app.state == NF_APP_STATE_WAITING_REPLY);

    /* A turn cancelled with PWR: its late voice is not played */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_PWR_CLICK);
    assert(app.state == NF_APP_STATE_IDLE);
    assert(!nf_app_on_voice_audio(&app, "vt_cancelled", 0));
    printf("[PASS] a turn that ends without voice returns to idle; late voice still plays\n");
}

static void test_link_drop_lets_the_answer_finish(void)
{
    static nf_app_ctx_t app;
    new_app(&app);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 20, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_on_speech_final(&app, app.pending[0].id, "讲个长故事", true);
    assert(nf_app_on_voice_audio(&app, "vt_7", 0));
    assert(nf_app_on_voice_audio(&app, "vt_7", 1));

    /* The relay resets the socket mid-answer: what arrived keeps playing */
    nf_app_set_link(&app, false);
    assert(s_cap.stops == 0);
    assert(app.state == NF_APP_STATE_PLAYING);
    /* Back within the answer: later sentences of the same turn still play */
    nf_app_set_link(&app, true);
    assert(nf_app_on_voice_audio(&app, "vt_7", 2));
    /* No voice_done will come for it; running dry is the end */
    nf_app_on_playback_drained(&app);
    assert(app.state == NF_APP_STATE_IDLE);

    /* BOOT while still offline mid-answer stops it here */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 20, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_on_speech_final(&app, app.pending[0].id, "再讲一个", true);
    assert(nf_app_on_voice_audio(&app, "vt_8", 0));
    nf_app_set_link(&app, false);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    assert(s_cap.stops == 1 && app.state == NF_APP_STATE_IDLE);
    nf_app_set_link(&app, true);

    /* Waiting for an answer that will not come now: back to idle at once */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 20, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_set_link(&app, false);
    assert(app.state == NF_APP_STATE_IDLE);
    printf("[PASS] a dropped link lets the answer already here finish\n");
}

/* Start a turn and get one sentence of its answer playing */
static void answer_playing(nf_app_ctx_t *app, const char *turn)
{
    nf_app_on_key_event(app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(app, 20, true);
    nf_app_on_key_event(app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_on_speech_final(app, app->pending[0].id, "问题", true);
    assert(nf_app_on_voice_audio(app, turn, 0));
    assert(app->state == NF_APP_STATE_PLAYING);
}

static void test_playing_always_ends(void)
{
    static nf_app_ctx_t app;
    new_app(&app);

    /* voice_done after the speaker already ran dry */
    answer_playing(&app, "vt_1");
    nf_app_tick(&app, 1000ULL);
    nf_app_on_playback_drained(&app);
    assert(app.state == NF_APP_STATE_PLAYING); /* more may come */
    nf_app_on_voice_done(&app, "vt_1");
    assert(app.state == NF_APP_STATE_IDLE);
    assert(!nf_app_on_voice_audio(&app, "vt_other", 0)); /* spoken and done: nothing late */

    /* The link drops after the speaker ran dry */
    answer_playing(&app, "vt_2");
    nf_app_on_playback_drained(&app);
    nf_app_set_link(&app, false);
    assert(app.state == NF_APP_STATE_IDLE);
    nf_app_set_link(&app, true);

    /* The rest of the answer is lost on the way: give up after a stall */
    answer_playing(&app, "vt_3");
    nf_app_tick(&app, 5000ULL);
    nf_app_on_playback_drained(&app);
    nf_app_tick(&app, 5000ULL + NF_PLAYBACK_STALL_MS - 1ULL);
    assert(app.state == NF_APP_STATE_PLAYING);
    /* a late sentence restarts the wait */
    assert(nf_app_on_voice_audio(&app, "vt_3", 1));
    nf_app_tick(&app, 5000ULL + NF_PLAYBACK_STALL_MS + 10ULL);
    assert(app.state == NF_APP_STATE_PLAYING);
    nf_app_on_playback_drained(&app);
    nf_app_tick(&app, 5000ULL + 2ULL * NF_PLAYBACK_STALL_MS + 20ULL);
    assert(app.state == NF_APP_STATE_IDLE);

    /* One sentence failing mid-answer does not end it */
    answer_playing(&app, "vt_4");
    nf_app_on_voice_failed(&app, "vt_4", "sentence synthesis failed");
    nf_app_on_playback_drained(&app);
    assert(app.state == NF_APP_STATE_PLAYING);
    assert(nf_app_on_voice_audio(&app, "vt_4", 2));
    nf_app_on_voice_done(&app, "vt_4");
    nf_app_on_playback_drained(&app);
    assert(app.state == NF_APP_STATE_IDLE);

    /* A text-only reply: voice_failed (fresh turn_id) right after stream_end ends the wait */
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_PRESS_START);
    feed(&app, 20, true);
    nf_app_on_key_event(&app, BSP_KEY_EVENT_BOOT_RELEASE);
    nf_app_on_speech_final(&app, app.pending[0].id, "你好", true);
    nf_app_on_turn_end(&app);
    nf_app_on_voice_failed(&app, "vt_fresh", "voice engine warming up");
    assert(app.state == NF_APP_STATE_IDLE);
    assert(strstr(app.screen_prompt, "还在启动"));
    printf("[PASS] playing always ends: late voice_done, drop after drain, lost tail\n");
}

static void test_reply_wav_to_16k(void)
{
    /* 24 kHz mono 16-bit WAV, 2400 samples of a constant */
    size_t n = 2400U;
    size_t len = 44U + n * 2U;
    uint8_t *wav = calloc(1, len);
    memcpy(wav, "RIFF", 4);
    uint32_t riff = (uint32_t)(len - 8U);
    memcpy(wav + 4, &riff, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16U;
    uint16_t pcm_fmt = 1U, ch = 1U, bits = 16U, align = 2U;
    uint32_t rate = 24000U, byte_rate = 48000U, data_len = (uint32_t)(n * 2U);
    memcpy(wav + 16, &fmt_len, 4);
    memcpy(wav + 20, &pcm_fmt, 2);
    memcpy(wav + 22, &ch, 2);
    memcpy(wav + 24, &rate, 4);
    memcpy(wav + 28, &byte_rate, 4);
    memcpy(wav + 32, &align, 2);
    memcpy(wav + 34, &bits, 2);
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &data_len, 4);
    for (size_t i = 0; i < n; ++i) {
        int16_t v = 1234;
        memcpy(wav + 44 + 2 * i, &v, 2);
    }
    static int16_t out[4000];
    size_t got = nf_reply_wav_to_16k(wav, len, out, 4000);
    assert(got >= 1598U && got <= 1600U);
    assert(out[got / 2U] == 1234);
    assert(nf_reply_wav_to_16k(wav, 20, out, 4000) == 0U);
    free(wav);
    printf("[PASS] 24 kHz WAV -> 16 kHz PCM\n");
}

static void test_addr_pick_order_and_dns(void)
{
    char host[64];
    assert(nf_addr_url_host("wss://relay.nevoflux.app", host, sizeof(host)) && strcmp(host, "relay.nevoflux.app") == 0);
    assert(nf_addr_url_host("wss://relay.x.app:8443/p?q", host, sizeof(host)) && strcmp(host, "relay.x.app") == 0);
    assert(!nf_addr_url_host("wss://", host, sizeof(host)));

    uint8_t q[128];
    size_t qlen = nf_addr_dns_query("relay.nevoflux.app", 0x1234, q, sizeof(q));
    static const uint8_t qname[] = "\x05relay\x08nevoflux\x03" "app";
    assert(qlen == 12 + sizeof(qname) + 4);
    assert(q[0] == 0x12 && q[1] == 0x34 && q[2] == 0x01 && q[5] == 1);
    assert(memcmp(q + 12, qname, sizeof(qname)) == 0); /* sizeof includes the root 0 */

    /* The reply: question echoed, a CNAME, then the two Cloudflare A records */
    uint8_t r[256];
    memcpy(r, q, qlen);
    r[2] = 0x81;
    r[3] = 0x80;
    r[7] = 3;
    size_t off = qlen;
    static const uint8_t cname[] = {0xC0, 12, 0, 5, 0, 1, 0, 0, 1, 0, 0, 2, 0xC0, 12};
    memcpy(r + off, cname, sizeof(cname));
    off += sizeof(cname);
    static const uint8_t a1[] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 1, 0, 0, 4, 104, 21, 63, 34};
    static const uint8_t a2[] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 1, 0, 0, 4, 172, 67, 169, 111};
    memcpy(r + off, a1, sizeof(a1));
    off += sizeof(a1);
    memcpy(r + off, a2, sizeof(a2));
    off += sizeof(a2);
    uint32_t ips[NF_ADDR_MAX];
    assert(nf_addr_dns_parse(r, off, 0x1234, ips, NF_ADDR_MAX) == 2);
    assert(memcmp(&ips[0], a1 + 12, 4) == 0 && memcmp(&ips[1], a2 + 12, 4) == 0);
    assert(nf_addr_dns_parse(r, off, 0x9999, ips, NF_ADDR_MAX) == -1); /* not our query */
    assert(nf_addr_dns_parse(r, off - 3, 0x1234, ips, NF_ADDR_MAX) == -1); /* cut short */
    assert(nf_addr_dns_parse(r, off, 0x1234, ips, 1) == 1);
    r[3] = 0x83; /* NXDOMAIN */
    assert(nf_addr_dns_parse(r, off, 0x1234, ips, NF_ADDR_MAX) == 0);

    /* Try order: the one that worked first; the one that just failed last */
    const uint32_t list[3] = {11, 22, 33};
    int o[NF_ADDR_MAX];
    assert(nf_addr_order(list, 3, 0, 0, o) == 3 && o[0] == 0 && o[1] == 1 && o[2] == 2);
    assert(nf_addr_order(list, 3, 22, 0, o) == 3 && o[0] == 1 && o[1] == 0 && o[2] == 2);
    assert(nf_addr_order(list, 3, 11, 11, o) == 3 && o[0] == 1 && o[1] == 2 && o[2] == 0);
    assert(nf_addr_order(list, 3, 33, 11, o) == 3 && o[0] == 2 && o[1] == 1 && o[2] == 0);
    assert(nf_addr_order(list, 3, 99, 0, o) == 3 && o[0] == 0); /* pinned no longer in DNS */
    printf("[PASS] address pick order and DNS parse\n");
}

int main(void)
{
    test_token_mgr_and_urls();
    test_ptt_chunks_and_turn();
    test_opus_uplink();
    test_tap_is_cancelled_and_silence_is_told();
    test_20s_split_joins_transcripts();
    test_barge_in_and_meeting();
    test_text_only_answer_times_out();
    test_link_drop_lets_the_answer_finish();
    test_playing_always_ends();
    test_reply_wav_to_16k();
    test_addr_pick_order_and_dns();
    printf("[PASS] All NevoFlux app unit tests passed!\n");
    return 0;
}
