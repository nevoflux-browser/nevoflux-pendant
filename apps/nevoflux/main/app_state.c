#include "nevoflux_app.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Records in a meeting's offline buffer */
enum { MEET_REC_JSON = 0, MEET_REC_CHUNK = 1, MEET_REC_END = 2 };
/*
 * The largest record, and the scratch to build or read one in. The scratch is
 * the end of the PSRAM block, not a static: internal RAM has no 6 KB to spare.
 */
#define MEET_REC_MAX  (NF_ID_CAP + 4U + NF_CHUNK_PCM_BYTES)
#define MEET_RING_CAP (NF_MEET_OFFLINE_BYTES - MEET_REC_MAX - 1U)

static uint8_t *meet_scratch(nf_app_ctx_t *app)
{
    return app->meet.mem + MEET_RING_CAP;
}

/* ------------------------------------------------------------------ helpers */

static void send_frame(nf_app_ctx_t *app, const char *json)
{
    if (app->io.send_frame && app->link_up) {
        app->io.send_frame(json, app->io.ctx);
    }
}

static void show_append(nf_app_ctx_t *app, const char *text);
static void show(nf_app_ctx_t *app, const char *text);
static void barge_in(nf_app_ctx_t *app);

/* ------------------------------------------------------------------ meeting stream */

static uint32_t meet_offset(const nf_app_ctx_t *app)
{
    return (uint32_t)(app->now_ms - app->meet.started_ms);
}

/* The meeting's frames queue behind what is buffered, so they reach the head in order */
static bool meet_buffering(const nf_app_ctx_t *app)
{
    return app->meet.mem && (!app->link_up || !nf_meet_buf_empty(&app->meet.buf));
}

static void meet_send(nf_app_ctx_t *app, const char *json)
{
    if (meet_buffering(app)) {
        (void)nf_meet_buf_put(&app->meet.buf, MEET_REC_JSON, meet_offset(app), (const uint8_t *)json, strlen(json));
    } else {
        send_frame(app, json);
    }
}

static void meet_notice(nf_app_ctx_t *app, const char *text)
{
    snprintf(app->meet.notice, sizeof(app->meet.notice), "%s", text);
    app->meet.notice_until_ms = app->now_ms + 3000ULL;
}

static void meet_release_buffer(nf_app_ctx_t *app)
{
    if (app->meet.mem && app->io.meet_free) {
        app->io.meet_free(app->meet.mem);
    }
    app->meet.mem = NULL;
    nf_meet_buf_init(&app->meet.buf, NULL, 0U);
}

/* One speech_chunk frame into json_scratch; its length, 0 if it does not fit */
static size_t chunk_json(nf_app_ctx_t *app, const char *id, uint32_t seq, const uint8_t *bytes, size_t len)
{
    int n = snprintf(app->json_scratch, sizeof(app->json_scratch),
                     "{\"kind\":\"speech_chunk\",\"utterance_id\":\"%s\",\"seq\":%u,\"%s\":\"",
                     id, (unsigned)seq, app->io.encode_frame ? "opus" : "pcm");
    if (n <= 0 || (size_t)n >= sizeof(app->json_scratch)) {
        return 0U;
    }
    size_t b = nf_b64_encode(bytes, len, app->json_scratch + n, sizeof(app->json_scratch) - (size_t)n);
    size_t end = (size_t)n + b;
    if (b == 0U || end + 3U > sizeof(app->json_scratch)) {
        return 0U;
    }
    memcpy(app->json_scratch + end, "\"}", 3U);
    return end + 2U;
}

static void meet_end_frame(nf_app_ctx_t *app, const char *id)
{
    char gaps[16U * 24U + 4U];
    (void)nf_meet_buf_gaps_json(&app->meet.buf, gaps, sizeof(gaps));
    char json[sizeof(gaps) + 128U];
    snprintf(json, sizeof(json), "{\"kind\":\"meeting_end\",\"meeting_id\":\"%s\",\"gaps\":%s}", id, gaps);
    send_frame(app, json);
}

/* Send up some of what was recorded offline; true while more is left */
static bool meet_replay(nf_app_ctx_t *app)
{
    uint8_t *rec = meet_scratch(app);
    const size_t rec_cap = MEET_REC_MAX + 1U;
    size_t budget = NF_MEET_REPLAY_BYTES;
    while (app->link_up && !nf_meet_buf_empty(&app->meet.buf) && budget > 0U) {
        uint8_t type;
        uint32_t off;
        long len = nf_meet_buf_peek(&app->meet.buf, &type, &off, rec, rec_cap - 1U);
        if (len < 0) {
            break;
        }
        size_t n = (size_t)len < rec_cap - 1U ? (size_t)len : rec_cap - 1U;
        rec[n] = 0;
        if (type == MEET_REC_JSON) {
            send_frame(app, (const char *)rec);
        } else if (type == MEET_REC_CHUNK) {
            size_t idl = strlen((const char *)rec); /* rec[n] is 0 */
            if (idl + 5U <= n) {
                const uint8_t *q = rec + idl + 1U;
                uint32_t seq = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
                if (chunk_json(app, (const char *)rec, seq, q + 4U, n - idl - 5U) > 0U) {
                    send_frame(app, app->json_scratch);
                }
            }
        } else if (type == MEET_REC_END) {
            meet_end_frame(app, (const char *)rec);
        }
        nf_meet_buf_pop(&app->meet.buf);
        budget = budget > n + NF_MEET_BUF_HDR ? budget - n - NF_MEET_BUF_HDR : 0U;
    }
    return !nf_meet_buf_empty(&app->meet.buf);
}

void nf_app_prompt(nf_app_ctx_t *app, const char *text)
{
    snprintf(app->screen_prompt, sizeof(app->screen_prompt), "%s", text ? text : "");
}

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
    if (app->meet.seg && meet_buffering(app)) {
        /* Kept as the bytes themselves: base64 and JSON would cost a third more room */
        uint8_t *rec = meet_scratch(app);
        size_t idl = strlen(app->utterance_id) + 1U;
        memcpy(rec, app->utterance_id, idl);
        uint32_t seq = app->chunk_seq;
        uint8_t sq[4] = {(uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)(seq >> 16), (uint8_t)(seq >> 24)};
        memcpy(rec + idl, sq, 4U);
        memcpy(rec + idl + 4U, app->chunk_pcm, app->chunk_len);
        (void)nf_meet_buf_put(&app->meet.buf, MEET_REC_CHUNK, meet_offset(app), rec, idl + 4U + app->chunk_len);
    } else if (chunk_json(app, app->utterance_id, app->chunk_seq, app->chunk_pcm, app->chunk_len) > 0U) {
        send_frame(app, app->json_scratch);
    }
    /* seq advances even if the frame was dropped: the head counts the hole in `gaps` */
    app->chunk_seq++;
    app->chunk_len = 0U;
    app->chunk_frames = 0U;
}

static void start_utterance(nf_app_ctx_t *app)
{
    uint64_t id = nf_utterance_gen_next(&app->utt_gen);
    (void)nf_utterance_id_to_str(id, app->utterance_id, sizeof(app->utterance_id));
    app->utterance_open = true;
    app->chunk_seq = 0U;
    app->chunk_len = 0U;
    app->chunk_frames = 0U;
    app->segment_frames = 0U;
    app->consecutive_silence_frames = 0U;

    if (app->io.encode_reset) {
        app->io.encode_reset(app->io.ctx);
    }
    /* A sentence of the meeting, not the wearer's question about it */
    app->meet.seg = app->meet.active && app->state == NF_APP_STATE_MEETING;
    app->meet.seg_power = 0.0;
    app->meet.seg_frames = 0U;
    char json[320];
    if (app->meet.seg) {
        snprintf(json, sizeof(json),
                 "{\"kind\":\"speech_start\",\"utterance_id\":\"%s\",\"sample_rate\":16000%s,"
                 "\"meeting_id\":\"%s\",\"offset_ms\":%u%s}",
                 app->utterance_id, app->io.encode_frame ? ",\"codec\":\"opus\",\"frame_ms\":20" : "",
                 app->meet.id, (unsigned)meet_offset(app), meet_buffering(app) ? ",\"late\":true" : "");
        meet_send(app, json);
        return;
    }
    snprintf(json, sizeof(json),
             "{\"kind\":\"speech_start\",\"utterance_id\":\"%s\",\"sample_rate\":16000%s}",
             app->utterance_id, app->io.encode_frame ? ",\"codec\":\"opus\",\"frame_ms\":20" : "");
    send_frame(app, json);
}

static void end_utterance(nf_app_ctx_t *app)
{
    if (!app->utterance_open) {
        return;
    }
    flush_chunk(app);
    char json[160];
    if (app->meet.seg) {
        /* How loud the sentence was: the head's guess at whether the wearer said it */
        double ms = app->meet.seg_frames ? app->meet.seg_power / (double)app->meet.seg_frames : 0.0;
        double db = ms > 0.0 ? 10.0 * log10(ms / (32768.0 * 32768.0)) : -96.0;
        snprintf(json, sizeof(json), "{\"kind\":\"speech_end\",\"utterance_id\":\"%s\",\"rms_db\":%.1f}",
                 app->utterance_id, db);
        meet_send(app, json);
    } else {
        snprintf(json, sizeof(json), "{\"kind\":\"speech_end\",\"utterance_id\":\"%s\"}", app->utterance_id);
        send_frame(app, json);
    }
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
    if (app->meet.seg) {
        meet_send(app, json);
    } else {
        send_frame(app, json);
    }
    app->utterance_open = false;
    app->chunk_len = 0U;
    app->chunk_frames = 0U;
}

static void push_frame(nf_app_ctx_t *app, const uint8_t *pcm)
{
    app->segment_frames++;
    if (app->meet.seg) {
        int16_t v[NF_PCM_SAMPLES_PER_FRAME];
        memcpy(v, pcm, sizeof(v));
        double sum = 0.0;
        for (size_t i = 0; i < NF_PCM_SAMPLES_PER_FRAME; ++i) {
            sum += (double)v[i] * (double)v[i];
        }
        app->meet.seg_power += sum / (double)NF_PCM_SAMPLES_PER_FRAME;
        app->meet.seg_frames++;
    }
    if (app->io.encode_frame) {
        /* One chunk per NF_CHUNK_FRAMES either way; Opus just makes it ~10x smaller */
        if (app->chunk_len + 2U + NF_OPUS_MAX_PACKET > sizeof(app->chunk_pcm)) {
            flush_chunk(app);
        }
        int16_t samples[NF_PCM_SAMPLES_PER_FRAME];
        memcpy(samples, pcm, sizeof(samples));
        size_t n = app->io.encode_frame(samples, app->chunk_pcm + app->chunk_len + 2U,
                                        NF_OPUS_MAX_PACKET, app->io.ctx);
        if (n > 0U && n <= NF_OPUS_MAX_PACKET) {
            app->chunk_pcm[app->chunk_len] = (uint8_t)(n & 0xFFU);
            app->chunk_pcm[app->chunk_len + 1U] = (uint8_t)(n >> 8);
            app->chunk_len += 2U + n;
        }
    } else {
        if (app->chunk_len + NF_PCM_BYTES_PER_FRAME > sizeof(app->chunk_pcm)) {
            flush_chunk(app);
        }
        memcpy(app->chunk_pcm + app->chunk_len, pcm, NF_PCM_BYTES_PER_FRAME);
        app->chunk_len += NF_PCM_BYTES_PER_FRAME;
    }
    app->chunk_frames++;
    if (app->chunk_frames >= NF_CHUNK_FRAMES) {
        flush_chunk(app);
    }
}

static void reset_answer(nf_app_ctx_t *app)
{
    app->turn_id[0] = '\0';
    app->sentences_queued = 0U;
    app->voice_done = false;
    app->speaker_dry = false;
    app->turn_ended = false;
    app->turn_end_ms = 0ULL;
}

/* The answer has been spoken (or given up on mid-way): nothing more of it is wanted. */
static void finish_answer(nf_app_ctx_t *app)
{
    reset_answer(app);
    app->submitted = false;
    if (app->meet.active) {
        /* A question in a meeting was answered: back to recording it */
        app->meet.answer_turn[0] = '\0';
        app->state = NF_APP_STATE_MEETING;
    } else {
        app->state = NF_APP_STATE_IDLE;
    }
}

static bool speaking(const nf_app_ctx_t *app)
{
    return app->state == NF_APP_STATE_PLAYING || app->state == NF_APP_STATE_MEETING_ANSWERING;
}

static void meeting_enter(nf_app_ctx_t *app)
{
    uint64_t id = nf_utterance_gen_next(&app->utt_gen);
    meet_release_buffer(app); /* anything left of the last meeting is given up */
    memset(&app->meet, 0, sizeof(app->meet));
    (void)nf_utterance_id_to_str(id, app->meet.id, sizeof(app->meet.id));
    app->meet.active = true;
    app->meet.started_ms = app->now_ms;
    if (app->io.meet_alloc) {
        app->meet.mem = (uint8_t *)app->io.meet_alloc(NF_MEET_OFFLINE_BYTES);
    }
    nf_meet_buf_init(&app->meet.buf, app->meet.mem, MEET_RING_CAP);
    char json[128];
    snprintf(json, sizeof(json), "{\"kind\":\"meeting_start\",\"meeting_id\":\"%s\"}", app->meet.id);
    send_frame(app, json);
    app->state = NF_APP_STATE_MEETING;
    app->utterance_open = false;
    app->consecutive_silence_frames = 0U;
    app->pending_count = 0U;
    show(app, "会议录音中…\n");
}

static void meeting_leave(nf_app_ctx_t *app)
{
    if (app->state == NF_APP_STATE_MEETING_ASKING) {
        cancel_utterance(app, "meeting_ended");
    } else if (app->state == NF_APP_STATE_MEETING_ANSWERING) {
        barge_in(app);
    }
    if (app->meet.seg) {
        end_utterance(app);
    }
    if (meet_buffering(app)) {
        (void)nf_meet_buf_put(&app->meet.buf, MEET_REC_END, meet_offset(app), (const uint8_t *)app->meet.id,
                              strlen(app->meet.id) + 1U);
    } else {
        meet_end_frame(app, app->meet.id);
    }
    app->meet.active = false;
    app->meet.seg = false;
    app->meet.ask_id[0] = '\0';
    app->meet.answer_turn[0] = '\0';
    app->pending_count = 0U;
    app->state = NF_APP_STATE_IDLE;
    if (nf_meet_buf_empty(&app->meet.buf)) {
        meet_release_buffer(app);
    }
    nf_app_prompt(app, app->link_up ? "会议已结束\n纪要生成中…" : "会议已结束\n连上电脑后补传");
}

/* Strip a leading run of spaces and punctuation (ASCII and full-width) */
static const char *skip_lead(const char *t)
{
    static const char *const marks[] = {"，", "。", "、", "：", "！", "？", "　", "“", "”"};
    for (;;) {
        if (*t == ' ' || *t == ',' || *t == '.' || *t == ':' || *t == '!' || *t == '?') {
            t++;
            continue;
        }
        bool skipped = false;
        for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) {
            size_t n = strlen(marks[i]);
            if (strncmp(t, marks[i], n) == 0) {
                t += n;
                skipped = true;
                break;
            }
        }
        if (!skipped) {
            return t;
        }
    }
}

bool nf_meeting_is_note(const char *text)
{
    static const char *const keys[] = {"标记", "记一下", "备注", "表记", "标及"};
    if (!text) {
        return false;
    }
    const char *t = skip_lead(text);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (strncmp(t, keys[i], strlen(keys[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* A question or note held down in a meeting came back as text */
static void meeting_ask_done(nf_app_ctx_t *app, const char *text)
{
    char id[NF_ID_CAP];
    snprintf(id, sizeof(id), "%s", app->meet.ask_id);
    app->meet.ask_id[0] = '\0';
    if (!text || !text[0]) {
        meet_notice(app, "没听清，请按住 BOOT 再说一次");
        return;
    }
    if (!app->link_up) {
        meet_notice(app, "未连接，问题未发送");
        return;
    }
    char esc[1100];
    nf_json_escape(text, esc, sizeof(esc));
    bool note = nf_meeting_is_note(text);
    int n = snprintf(app->json_scratch, sizeof(app->json_scratch),
                     "{\"kind\":\"%s\",\"meeting_id\":\"%s\",\"utterance_id\":\"%s\",\"text\":\"%s\",\"offset_ms\":%u}",
                     note ? "meeting_note" : "meeting_ask", app->meet.id, id, esc, (unsigned)app->meet.ask_offset_ms);
    if (n <= 0 || (size_t)n >= sizeof(app->json_scratch)) {
        return;
    }
    send_frame(app, app->json_scratch);
    if (note) {
        if (app->io.cue) {
            app->io.cue(app->io.ctx);
        }
        meet_notice(app, "已记下批注");
    } else {
        snprintf(app->meet.answer_turn, sizeof(app->meet.answer_turn), "%s", id);
        app->meet.answer_since_ms = app->now_ms;
        app->barged_turn_id[0] = '\0';
        meet_notice(app, "思考中…");
    }
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
        nf_app_prompt(app, "没听清，请按住 BOOT 再说一次");
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
    if (app->meet.active) {
        if (up) {
            /* A head that restarted forgot where to send this meeting's minutes */
            char json[128];
            snprintf(json, sizeof(json), "{\"kind\":\"meeting_start\",\"meeting_id\":\"%s\"}", app->meet.id);
            send_frame(app, json);
            return;
        }
        /* The meeting goes on into the offline buffer; a question in flight is lost */
        if (app->state == NF_APP_STATE_MEETING_ASKING) {
            app->utterance_open = false;
            meet_notice(app, "未连接，问题未发送");
        } else if (app->state == NF_APP_STATE_MEETING_ANSWERING) {
            if (app->io.stop_playback) {
                app->io.stop_playback(app->io.ctx);
            }
            reset_answer(app);
        }
        app->meet.ask_id[0] = '\0';
        app->meet.answer_turn[0] = '\0';
        app->state = NF_APP_STATE_MEETING;
        return;
    }
    if (!up) {
        /*
         * The relay resets sockets every few minutes on some networks. The head
         * synthesises ahead of the speaker, so most of an answer is usually
         * already queued here: let it finish. No voice_done will follow, so
         * running dry ends it; sentences of the same turn that arrive after a
         * quick reconnect still play.
         */
        if (app->state == NF_APP_STATE_PLAYING && !app->speaker_dry) {
            app->voice_done = true;
            app->utterance_open = false;
            app->pending_count = 0U;
            return;
        }
        /* Whatever else was in flight is gone with the socket */
        app->utterance_open = false;
        app->pending_count = 0U;
        app->ptt_accum_text[0] = '\0';
        reset_answer(app);
        if (app->state != NF_APP_STATE_IDLE) {
            show(app, "与电脑的连接断开了");
            nf_app_prompt(app, "与电脑的连接断开了");
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
    if (app->meet.mem && app->link_up && !nf_meet_buf_empty(&app->meet.buf)) {
        (void)meet_replay(app);
    }
    if (!app->meet.active && app->meet.mem && nf_meet_buf_empty(&app->meet.buf)) {
        meet_release_buffer(app); /* the last of an ended meeting went up */
    }
    if (app->meet.active) {
        if (app->meet.answer_turn[0] && app->state == NF_APP_STATE_MEETING &&
            now_ms - app->meet.answer_since_ms >= NF_VOICE_GRACE_MS) {
            app->meet.answer_turn[0] = '\0';
            meet_notice(app, "这次没有语音回复");
        }
        if (now_ms < app->meet.notice_until_ms) {
            nf_app_prompt(app, app->meet.notice);
        } else {
            unsigned s = (unsigned)((now_ms - app->meet.started_ms) / 1000ULL);
            char clock[16];
            if (s >= 3600U) {
                snprintf(clock, sizeof(clock), "%u:%02u:%02u", s / 3600U, s / 60U % 60U, s % 60U);
            } else {
                snprintf(clock, sizeof(clock), "%02u:%02u", s / 60U, s % 60U);
            }
            char line[96];
            if (app->link_up) {
                snprintf(line, sizeof(line), "会议录音中 %s\n已记录 %u 句", clock, (unsigned)app->meet.sentences);
            } else {
                snprintf(line, sizeof(line), "离线录音中 %s\n连上电脑后补传", clock);
            }
            nf_app_prompt(app, line);
        }
    }
    /* The head finished the turn but never spoke (voice off, engine cold, text-only) */
    if (app->state == NF_APP_STATE_WAITING_REPLY && app->turn_ended && app->sentences_queued == 0U &&
        now_ms - app->turn_end_ms >= NF_VOICE_GRACE_MS) {
        reset_answer(app);
        nf_app_prompt(app, "这次没有语音回复\n按住 BOOT 再说一次");
        app->state = NF_APP_STATE_IDLE;
    }
    if (speaking(app) && app->speaker_dry &&
        now_ms - app->dry_since_ms >= NF_PLAYBACK_STALL_MS) {
        finish_answer(app);
    }
}

void nf_app_on_key_event(nf_app_ctx_t *app, bsp_key_event_t ev)
{
    if (!app) {
        return;
    }

    switch (ev) {
    case BSP_KEY_EVENT_BOOT_PRESS_START:
        if (app->meet.active) {
            /* A question about the meeting, or a note in it; the meeting keeps going */
            if (!app->link_up) {
                meet_notice(app, "未连接，问题未发送");
                return;
            }
            if (app->state == NF_APP_STATE_MEETING_ANSWERING) {
                barge_in(app);
            }
            if (app->meet.seg) {
                end_utterance(app);
            }
            app->meet.answer_turn[0] = '\0';
            app->state = NF_APP_STATE_MEETING_ASKING;
            app->press_frames = 0U;
            app->meet.ask_offset_ms = meet_offset(app);
            start_utterance(app);
            return;
        }
        if (!app->link_up) {
            if (app->state == NF_APP_STATE_PLAYING) {
                /* Still a way to stop the answer; the head is told nothing */
                if (app->io.stop_playback) {
                    app->io.stop_playback(app->io.ctx);
                }
                reset_answer(app);
                app->state = NF_APP_STATE_IDLE;
            }
            show(app, "未连接电脑，暂时无法对话");
            nf_app_prompt(app, "未连接电脑，暂时无法对话");
            return;
        }
        if (app->state == NF_APP_STATE_PLAYING ||
            (app->state == NF_APP_STATE_WAITING_REPLY && app->turn_id[0])) {
            barge_in(app);
        }
        app->state = NF_APP_STATE_LISTENING;
        app->ptt_accum_text[0] = '\0';
        app->pending_count = 0U;
        app->press_frames = 0U;
        app->submitted = false;
        show(app, "");
        nf_app_prompt(app, NULL);
        start_utterance(app);
        break;

    case BSP_KEY_EVENT_BOOT_RELEASE:
        if (app->state == NF_APP_STATE_MEETING_ASKING) {
            if (app->press_frames < NF_MIN_UTTERANCE_FRAMES) {
                cancel_utterance(app, "too_short");
            } else {
                snprintf(app->meet.ask_id, sizeof(app->meet.ask_id), "%s", app->utterance_id);
                end_utterance(app);
            }
            app->state = NF_APP_STATE_MEETING;
            break;
        }
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
        if (app->state == NF_APP_STATE_MEETING_ASKING) {
            cancel_utterance(app, "user_stopped");
            app->state = NF_APP_STATE_MEETING;
            break;
        }
        if (app->state == NF_APP_STATE_MEETING_ANSWERING) {
            barge_in(app);
            app->meet.answer_turn[0] = '\0';
            app->state = NF_APP_STATE_MEETING;
            break;
        }
        if (app->state == NF_APP_STATE_LISTENING || app->state == NF_APP_STATE_WAITING_REPLY ||
            app->state == NF_APP_STATE_PLAYING) {
            /* Cancel this turn: nothing more is sent, the answer stops */
            cancel_utterance(app, "user_stopped");
            if (app->state == NF_APP_STATE_PLAYING || app->turn_id[0]) {
                barge_in(app);
            }
            app->ptt_accum_text[0] = '\0';
            app->pending_count = 0U;
            app->submitted = false; /* its answer is not wanted, however late */
            app->state = NF_APP_STATE_IDLE;
        }
        break;

    case BSP_KEY_EVENT_BOOT_DOUBLE_CLICK:
        /* Toggle hands-free Meeting Mode */
        if (app->meet.active) {
            meeting_leave(app);
        } else if (app->state == NF_APP_STATE_IDLE && app->link_up) {
            meeting_enter(app);
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

    if (app->state == NF_APP_STATE_MEETING_ASKING) {
        if (!app->utterance_open) {
            return;
        }
        app->press_frames++;
        push_frame(app, pcm_640);
        if (app->segment_frames >= NF_PTT_MAX_FRAMES_PER_SEG) {
            /* Held too long for a question: take what there is */
            nf_app_on_key_event(app, BSP_KEY_EVENT_BOOT_RELEASE);
        }
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
    if (app->meet.active && app->meet.ask_id[0] && utterance_id && strcmp(utterance_id, app->meet.ask_id) == 0) {
        (void)pending_finish(app, utterance_id, kept);
        meeting_ask_done(app, kept);
        return;
    }
    if (!pending_finish(app, utterance_id, kept)) {
        return; /* a cancelled or superseded utterance */
    }
    if (app->meet.active) {
        if (kept[0]) {
            app->meet.sentences++;
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
    if (app && app->meet.active && app->meet.ask_id[0] && utterance_id &&
        strcmp(utterance_id, app->meet.ask_id) == 0) {
        app->meet.ask_id[0] = '\0';
        meet_notice(app, "语音识别失败，请再说一次");
        return;
    }
    if (!app || !pending_finish(app, utterance_id, "")) {
        return;
    }
    if (app->state == NF_APP_STATE_WAITING_REPLY && pending_outstanding(app) == 0U) {
        pending_join(app);
        if (app->ptt_accum_text[0] == '\0') {
            char line[256];
            snprintf(line, sizeof(line), "语音识别失败：%s", message ? message : "");
            show(app, line);
            nf_app_prompt(app, "语音识别失败，请再说一次");
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
        /* More text after a stream_end (agent mode: text, a tool, more text) */
        app->turn_ended = false;
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
        nf_app_prompt(app, "出错了，请再说一次");
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
    if (app->meet.active) {
        /* Only the answer to the question asked; the meeting pauses while it plays */
        if (app->state == NF_APP_STATE_MEETING && app->meet.answer_turn[0] &&
            strcmp(turn_id, app->meet.answer_turn) == 0) {
            if (app->meet.seg) {
                end_utterance(app);
            }
            reset_answer(app);
            snprintf(app->turn_id, sizeof(app->turn_id), "%s", turn_id);
            app->state = NF_APP_STATE_MEETING_ANSWERING;
        }
        if (app->state != NF_APP_STATE_MEETING_ANSWERING || strcmp(turn_id, app->turn_id) != 0) {
            return false;
        }
        app->sentences_queued++;
        app->speaker_dry = false;
        return true;
    }
    /* Waiting for it, or given up on it but nothing asked since (a late first sentence) */
    if ((app->state == NF_APP_STATE_WAITING_REPLY || app->state == NF_APP_STATE_IDLE) &&
        !app->turn_id[0] && app->submitted) {
        snprintf(app->turn_id, sizeof(app->turn_id), "%s", turn_id);
        app->state = NF_APP_STATE_PLAYING;
    }
    if (app->state != NF_APP_STATE_PLAYING || strcmp(turn_id, app->turn_id) != 0) {
        return false;
    }
    app->sentences_queued++;
    app->speaker_dry = false;
    return true;
}

void nf_app_on_voice_done(nf_app_ctx_t *app, const char *turn_id)
{
    if (!app || !turn_id) {
        return;
    }
    if (strcmp(turn_id, app->turn_id) == 0) {
        app->voice_done = true;
        if (speaking(app) && app->speaker_dry) {
            finish_answer(app); /* it all played before this arrived */
        }
    }
}

void nf_app_on_voice_failed(nf_app_ctx_t *app, const char *turn_id, const char *message)
{
    if (!app) {
        return;
    }
    if (app->meet.active) {
        if (app->meet.answer_turn[0] && turn_id && strcmp(turn_id, app->meet.answer_turn) == 0 &&
            app->state == NF_APP_STATE_MEETING) {
            app->meet.answer_turn[0] = '\0';
            meet_notice(app, "语音合成失败");
        }
        return;
    }
    /*
     * Mid-answer it is one sentence that failed: the head goes on with the next
     * and ends the turn with voice_done as usual. Only while nothing has been
     * spoken yet is it the end: the head sends one (with a fresh turn_id) after
     * stream_end when the reply goes out text-only, e.g. its engine still loading.
     * Its later sentences, if any, still play (a late first sentence).
     */
    if (app->state != NF_APP_STATE_WAITING_REPLY || app->turn_id[0]) {
        return;
    }
    show_append(app, "\n（语音合成失败");
    if (message && message[0]) {
        show_append(app, "：");
        show_append(app, message);
    }
    show_append(app, "）");
    nf_app_prompt(app, (message && strstr(message, "warming")) ? "电脑的语音还在启动\n请稍后再说一次"
                                                             : "语音合成失败");
    reset_answer(app);
    app->state = NF_APP_STATE_IDLE;
}

void nf_app_on_playback_drained(nf_app_ctx_t *app)
{
    if (!app) {
        return;
    }
    if (!speaking(app)) {
        return;
    }
    if (app->voice_done) {
        finish_answer(app);
        return;
    }
    app->speaker_dry = true;
    app->dry_since_ms = app->now_ms;
}

void nf_app_on_meeting_summary(nf_app_ctx_t *app, const char *meeting_id, uint32_t todos)
{
    (void)meeting_id;
    if (!app || app->state != NF_APP_STATE_IDLE) {
        return;
    }
    char line[96];
    snprintf(line, sizeof(line), "纪要已生成，共 %u 条待办\n在电脑上查看", (unsigned)todos);
    nf_app_prompt(app, line);
}
