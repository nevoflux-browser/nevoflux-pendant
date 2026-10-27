#include "nevoflux_app.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static void send_frame(nf_app_ctx_t *app, const char *json)
{
    if (app->io.send_frame && app->link_up) {
        app->io.send_frame(json, app->io.ctx);
    }
}

static void show_append(nf_app_ctx_t *app, const char *text);

static void show(nf_app_ctx_t *app, const char *text)
{
    app->screen_transcript[0] = '\0';
    show_append(app, text);
}

/* Append to the screen, keeping the tail when it overflows (never splitting UTF-8). */
static void show_append(nf_app_ctx_t *app, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    char *buf = app->screen_transcript;
    size_t cap = sizeof(app->screen_transcript);
    size_t cur = strlen(buf);
    size_t add = strlen(text);
    if (add >= cap) {
        text += add - (cap - 1U);
        add = cap - 1U;
        cur = 0U;
    } else if (cur + add >= cap) {
        size_t drop = cur + add - (cap - 1U);
        while (drop < cur && ((unsigned char)buf[drop] & 0xC0U) == 0x80U) {
            drop++;
        }
        memmove(buf, buf + drop, cur - drop + 1U);
        cur -= drop;
    }
    while (add > 0U && ((unsigned char)text[0] & 0xC0U) == 0x80U) {
        text++;
        add--;
    }
    memcpy(buf + cur, text, add);
    buf[cur + add] = '\0';
}

static void pending_add(nf_app_ctx_t *app, const char *id)
{
    if (app->pending_count >= NF_MAX_PENDING_FINALS) {
        memmove(&app->pending[0], &app->pending[1], (NF_MAX_PENDING_FINALS - 1U) * sizeof(app->pending[0]));
        app->pending_count--;
    }
    snprintf(app->pending[app->pending_count].id, NF_ID_CAP, "%s", id);
    app->pending[app->pending_count].done = false;
    app->pending[app->pending_count].text[0] = '\0';
    app->pending_count++;
}

/* Record `id`'s transcript. False if it is not one this press is waiting for. */
static bool pending_finish(nf_app_ctx_t *app, const char *id, const char *text)
{
    for (size_t i = 0; id && i < app->pending_count; ++i) {
        if (!app->pending[i].done && strcmp(app->pending[i].id, id) == 0) {
            app->pending[i].done = true;
            snprintf(app->pending[i].text, sizeof(app->pending[i].text), "%s", text ? text : "");
            return true;
        }
    }
    return false;
}

static size_t pending_outstanding(const nf_app_ctx_t *app)
{
    size_t n = 0U;
    for (size_t i = 0; i < app->pending_count; ++i) {
        n += app->pending[i].done ? 0U : 1U;
    }
    return n;
}

/* The transcripts in speaking order, whatever order they came back in. */
static void pending_join(nf_app_ctx_t *app)
{
    size_t cur = 0U;
    app->ptt_accum_text[0] = '\0';
    for (size_t i = 0; i < app->pending_count; ++i) {
        if (app->pending[i].done && cur < sizeof(app->ptt_accum_text) - 1U) {
            snprintf(app->ptt_accum_text + cur, sizeof(app->ptt_accum_text) - cur, "%s", app->pending[i].text);
            cur = strlen(app->ptt_accum_text);
        }
    }
}

static void flush_chunk(nf_app_ctx_t *app)
{
    if (!app->utterance_open || app->chunk_len == 0U) {
        return;
    }
    int n = snprintf(app->json_scratch, sizeof(app->json_scratch),
                     "{\"kind\":\"speech_chunk\",\"utterance_id\":\"%s\",\"seq\":%u,\"pcm\":\"",
                     app->utterance_id, (unsigned)app->chunk_seq);
    if (n > 0 && (size_t)n < sizeof(app->json_scratch)) {
        size_t b = nf_b64_encode(app->chunk_pcm, app->chunk_len,
                                 app->json_scratch + n, sizeof(app->json_scratch) - (size_t)n);
        size_t end = (size_t)n + b;
        if (b > 0U && end + 3U <= sizeof(app->json_scratch)) {
            memcpy(app->json_scratch + end, "\"}", 3U);
            send_frame(app, app->json_scratch);
        }
    }
    /* seq advances even if the frame was dropped: the head counts the hole in `gaps` */
    app->chunk_seq++;
    app->chunk_len = 0U;
}

static void start_utterance(nf_app_ctx_t *app)
{
    uint64_t id = nf_utterance_gen_next(&app->utt_gen);
    (void)nf_utterance_id_to_str(id, app->utterance_id, sizeof(app->utterance_id));
    app->utterance_open = true;
    app->chunk_seq = 0U;
    app->chunk_len = 0U;
    app->segment_frames = 0U;
    app->consecutive_silence_frames = 0U;

    char json[160];
    snprintf(json, sizeof(json),
             "{\"kind\":\"speech_start\",\"utterance_id\":\"%s\",\"sample_rate\":16000}",
             app->utterance_id);
    send_frame(app, json);
}

static void end_utterance(nf_app_ctx_t *app)
{
    if (!app->utterance_open) {
        return;
    }
    flush_chunk(app);
    char json[128];
    snprintf(json, sizeof(json), "{\"kind\":\"speech_end\",\"utterance_id\":\"%s\"}", app->utterance_id);
    send_frame(app, json);
    pending_add(app, app->utterance_id);
    app->utterance_open = false;
}

static void cancel_utterance(nf_app_ctx_t *app, const char *reason)
{
    if (!app->utterance_open) {
        return;
    }
    char json[160];
    snprintf(json, sizeof(json),
             "{\"kind\":\"speech_cancel\",\"utterance_id\":\"%s\",\"reason\":\"%s\"}",
             app->utterance_id, reason);
    send_frame(app, json);
    app->utterance_open = false;
    app->chunk_len = 0U;
}

static void push_frame(nf_app_ctx_t *app, const uint8_t *pcm)
{
    if (app->chunk_len + NF_PCM_BYTES_PER_FRAME > sizeof(app->chunk_pcm)) {
        flush_chunk(app);
    }
    memcpy(app->chunk_pcm + app->chunk_len, pcm, NF_PCM_BYTES_PER_FRAME);
    app->chunk_len += NF_PCM_BYTES_PER_FRAME;
    app->segment_frames++;
    if (app->chunk_len >= sizeof(app->chunk_pcm)) {
        flush_chunk(app);
    }
}

static void reset_answer(nf_app_ctx_t *app)
{
    app->turn_id[0] = '\0';
    app->sentences_queued = 0U;
    app->voice_done = false;
    app->turn_ended = false;
    app->turn_end_ms = 0ULL;
}

/* Stop the answer being spoken, here first and then at the head (barge-in). */
static void barge_in(nf_app_ctx_t *app)
{
    if (app->io.stop_playback) {
        app->io.stop_playback(app->io.ctx);
    }
    if (app->turn_id[0]) {
        char json[160];
        uint32_t played = app->sentences_queued > 0U ? app->sentences_queued - 1U : 0U;
        snprintf(json, sizeof(json),
                 "{\"kind\":\"voice_barge_in\",\"turn_id\":\"%s\",\"played\":%u}",
                 app->turn_id, (unsigned)played);
        send_frame(app, json);
        snprintf(app->barged_turn_id, sizeof(app->barged_turn_id), "%s", app->turn_id);
    }
    reset_answer(app);
}

static void submit_turn(nf_app_ctx_t *app)
{
    char esc[1100];
    nf_json_escape(app->ptt_accum_text, esc, sizeof(esc));
    int n = snprintf(app->json_scratch, sizeof(app->json_scratch),
                     "{\"kind\":\"user_message\",\"text\":\"%s\",\"voice\":true,\"voice_codec\":\"opus\"}",
                     esc);
    if (n > 0 && (size_t)n < sizeof(app->json_scratch)) {
        send_frame(app, app->json_scratch);
    }
    reset_answer(app);
    app->submitted = true;
    app->barged_turn_id[0] = '\0';
    char line[1100];
    snprintf(line, sizeof(line), "你：%s\n", app->ptt_accum_text);
    show(app, line);
    app->ptt_accum_text[0] = '\0';
}

/* All segments of a released push-to-talk are in: send the turn, or give up. */
static void maybe_complete_ptt(nf_app_ctx_t *app)
{
    if (app->state != NF_APP_STATE_WAITING_REPLY || app->submitted || pending_outstanding(app) > 0U ||
        app->utterance_open) {
        return;
    }
    pending_join(app);
    app->pending_count = 0U;
    if (app->ptt_accum_text[0] != '\0') {
        submit_turn(app);
    } else {
        show(app, "没听清，请按住 BOOT 再说一次");
        app->state = NF_APP_STATE_IDLE;
    }
}

/* ------------------------------------------------------------------ API */

void nf_app_init(nf_app_ctx_t *app, uint32_t boot_nonce, const nf_app_io_t *io)
{
    if (!app) {
        return;
    }
    memset(app, 0, sizeof(*app));
    app->state = NF_APP_STATE_IDLE;
    nf_utterance_gen_init(&app->utt_gen, boot_nonce);
    if (io) {
        app->io = *io;
    }
}

void nf_app_set_link(nf_app_ctx_t *app, bool up)
{
    if (!app || app->link_up == up) {
        return;
    }
    app->link_up = up;
    if (!up) {
        /* Whatever was in flight is gone with the socket */
        if (app->state == NF_APP_STATE_PLAYING && app->io.stop_playback) {
            app->io.stop_playback(app->io.ctx);
        }
        app->utterance_open = false;
        app->pending_count = 0U;
        app->ptt_accum_text[0] = '\0';
        reset_answer(app);
        if (app->state != NF_APP_STATE_IDLE) {
            show(app, "与电脑的连接断开了");
        }
        app->state = NF_APP_STATE_IDLE;
    }
}

void nf_app_tick(nf_app_ctx_t *app, uint64_t now_ms)
{
    if (!app) {
        return;
    }
    app->now_ms = now_ms;
    /* The head finished the turn but never spoke (voice off, engine cold, text-only) */
    if (app->state == NF_APP_STATE_WAITING_REPLY && app->turn_ended && app->sentences_queued == 0U &&
        now_ms - app->turn_end_ms >= NF_VOICE_GRACE_MS) {
        reset_answer(app);
        app->state = NF_APP_STATE_IDLE;
    }
}

void nf_app_on_key_event(nf_app_ctx_t *app, bsp_key_event_t ev)
{
    if (!app) {
        return;
    }

    switch (ev) {
    case BSP_KEY_EVENT_BOOT_PRESS_START:
        if (!app->link_up) {
            show(app, "未连接电脑，暂时无法对话");
            return;
        }
        if (app->state == NF_APP_STATE_PLAYING ||
            (app->state == NF_APP_STATE_WAITING_REPLY && app->turn_id[0])) {
            barge_in(app);
        } else if (app->state == NF_APP_STATE_MEETING) {
            end_utterance(app);
        }
        app->state = NF_APP_STATE_LISTENING;
        app->ptt_accum_text[0] = '\0';
        app->pending_count = 0U;
        app->press_frames = 0U;
        app->submitted = false;
        show(app, "");
        start_utterance(app);
        break;

    case BSP_KEY_EVENT_BOOT_RELEASE:
        if (app->state == NF_APP_STATE_LISTENING) {
            if (app->press_frames < NF_MIN_UTTERANCE_FRAMES && app->pending_count == 0U) {
                cancel_utterance(app, "too_short");
                app->state = NF_APP_STATE_IDLE;
                break;
            }
            end_utterance(app);
            app->state = NF_APP_STATE_WAITING_REPLY;
            reset_answer(app);
        }
        break;

    case BSP_KEY_EVENT_PWR_CLICK:
        if (app->state == NF_APP_STATE_LISTENING || app->state == NF_APP_STATE_WAITING_REPLY ||
            app->state == NF_APP_STATE_PLAYING) {
            /* Cancel this turn: nothing more is sent, the answer stops */
            cancel_utterance(app, "user_stopped");
            if (app->state == NF_APP_STATE_PLAYING || app->turn_id[0]) {
                barge_in(app);
            }
            app->ptt_accum_text[0] = '\0';
            app->pending_count = 0U;
            app->state = NF_APP_STATE_IDLE;
        }
        break;

    case BSP_KEY_EVENT_BOOT_DOUBLE_CLICK:
        /* Toggle hands-free Meeting Mode */
        if (app->state == NF_APP_STATE_MEETING) {
            end_utterance(app);
            app->state = NF_APP_STATE_IDLE;
        } else if (app->state == NF_APP_STATE_IDLE && app->link_up) {
            app->state = NF_APP_STATE_MEETING;
            app->utterance_open = false;
            app->consecutive_silence_frames = 0U;
            app->pending_count = 0U;
            show(app, "会议录音中…\n");
        }
        break;

    default:
        break;
    }
}

void nf_app_on_mic_frame(nf_app_ctx_t *app, const uint8_t pcm_640[NF_PCM_BYTES_PER_FRAME], bool is_speech)
{
    if (!app || !pcm_640) {
        return;
    }

    if (app->state == NF_APP_STATE_LISTENING) {
        /*
         * Push-to-talk: the button decides start and end, so pauses never cut
         * the turn short. Past 20 s, the next VAD silence splits it into a new
         * utterance (SenseVoice cost grows with the square of the length); the
         * transcripts are joined locally and sent as one turn on release.
         */
        if (!app->utterance_open) {
            start_utterance(app);
        }
        app->press_frames++;
        push_frame(app, pcm_640);
        if (!is_speech && app->segment_frames >= NF_PTT_MAX_FRAMES_PER_SEG) {
            end_utterance(app);
            start_utterance(app);
        }
        return;
    }

    if (app->state == NF_APP_STATE_MEETING) {
        /* Hands-free: VAD opens an utterance on speech, 800 ms of silence closes it */
        if (is_speech) {
            app->consecutive_silence_frames = 0U;
            if (!app->utterance_open) {
                start_utterance(app);
            }
            push_frame(app, pcm_640);
        } else if (app->utterance_open) {
            app->consecutive_silence_frames++;
            push_frame(app, pcm_640);
            if (app->consecutive_silence_frames >= NF_MEETING_SILENCE_FRAMES ||
                app->segment_frames >= NF_PTT_MAX_FRAMES_PER_SEG) {
                end_utterance(app);
            }
        }
    }
}

void nf_app_on_speech_partial(nf_app_ctx_t *app, const char *utterance_id, const char *text)
{
    if (!app || !text) {
        return;
    }
    if (app->state == NF_APP_STATE_LISTENING &&
        utterance_id && strcmp(utterance_id, app->utterance_id) == 0) {
        pending_join(app);
        show(app, app->ptt_accum_text);
        show_append(app, text);
    }
}

void nf_app_on_speech_final(nf_app_ctx_t *app, const char *utterance_id, const char *text, bool accepted)
{
    if (!app) {
        return;
    }
    const char *kept = (accepted && text) ? text : "";
    if (!pending_finish(app, utterance_id, kept)) {
        return; /* a cancelled or superseded utterance */
    }
    if (app->state == NF_APP_STATE_MEETING) {
        if (kept[0]) {
            show_append(app, kept);
            show_append(app, "\n");
        }
        /* Shown as each lands; only this one is forgotten, others are still out */
        size_t keep = 0U;
        for (size_t i = 0; i < app->pending_count; ++i) {
            if (!app->pending[i].done) {
                app->pending[keep++] = app->pending[i];
            }
        }
        app->pending_count = keep;
        return;
    }
    if (app->state == NF_APP_STATE_LISTENING) {
        pending_join(app);
        show(app, app->ptt_accum_text);
    }
    maybe_complete_ptt(app);
}

void nf_app_on_speech_error(nf_app_ctx_t *app, const char *utterance_id, const char *message)
{
    if (!app || !pending_finish(app, utterance_id, "")) {
        return;
    }
    if (app->state == NF_APP_STATE_WAITING_REPLY && pending_outstanding(app) == 0U) {
        pending_join(app);
        if (app->ptt_accum_text[0] == '\0') {
            char line[256];
            snprintf(line, sizeof(line), "语音识别失败：%s", message ? message : "");
            show(app, line);
            app->pending_count = 0U;
            app->state = NF_APP_STATE_IDLE;
            return;
        }
    }
    maybe_complete_ptt(app);
}

void nf_app_on_stream_delta(nf_app_ctx_t *app, const char *delta)
{
    if (!app || !delta) {
        return;
    }
    /* Only the answer to what this pendant asked; a replayed transcript is not news */
    if (app->state == NF_APP_STATE_WAITING_REPLY || app->state == NF_APP_STATE_PLAYING) {
        show_append(app, delta);
    }
}

void nf_app_on_turn_end(nf_app_ctx_t *app)
{
    if (!app) {
        return;
    }
    if (app->state == NF_APP_STATE_WAITING_REPLY || app->state == NF_APP_STATE_PLAYING) {
        app->turn_ended = true;
        app->turn_end_ms = app->now_ms;
    }
}

void nf_app_on_error(nf_app_ctx_t *app, const char *message)
{
    if (!app) {
        return;
    }
    if (app->state == NF_APP_STATE_WAITING_REPLY) {
        char line[256];
        snprintf(line, sizeof(line), "出错了：%s", message ? message : "");
        show(app, line);
        reset_answer(app);
        app->state = NF_APP_STATE_IDLE;
    }
}

bool nf_app_on_voice_audio(nf_app_ctx_t *app, const char *turn_id, uint32_t seq)
{
    (void)seq;
    if (!app || !turn_id || !turn_id[0]) {
        return false;
    }
    if (app->barged_turn_id[0] && strcmp(turn_id, app->barged_turn_id) == 0) {
        return false;
    }
    if (app->state == NF_APP_STATE_WAITING_REPLY && !app->turn_id[0] && app->submitted) {
        snprintf(app->turn_id, sizeof(app->turn_id), "%s", turn_id);
        app->state = NF_APP_STATE_PLAYING;
    }
    if (app->state != NF_APP_STATE_PLAYING || strcmp(turn_id, app->turn_id) != 0) {
        return false;
    }
    app->sentences_queued++;
    return true;
}

void nf_app_on_voice_done(nf_app_ctx_t *app, const char *turn_id)
{
    if (!app || !turn_id) {
        return;
    }
    if (strcmp(turn_id, app->turn_id) == 0) {
        app->voice_done = true;
    }
}

void nf_app_on_voice_failed(nf_app_ctx_t *app, const char *turn_id, const char *message)
{
    if (!app) {
        return;
    }
    if ((app->state == NF_APP_STATE_WAITING_REPLY && !app->turn_id[0]) ||
        (turn_id && strcmp(turn_id, app->turn_id) == 0)) {
        show_append(app, "\n（语音合成失败");
        if (message && message[0]) {
            show_append(app, "：");
            show_append(app, message);
        }
        show_append(app, "）");
        app->voice_done = true;
        if (app->state == NF_APP_STATE_WAITING_REPLY) {
            reset_answer(app);
            app->state = NF_APP_STATE_IDLE;
        }
    }
}

void nf_app_on_playback_drained(nf_app_ctx_t *app)
{
    if (!app) {
        return;
    }
    if (app->state == NF_APP_STATE_PLAYING && app->voice_done) {
        reset_answer(app);
        app->state = NF_APP_STATE_IDLE;
    }
}
