#include "nevoflux_app.h"
#include <stdio.h>
#include <string.h>

static void enqueue_nfa1_frame(nf_app_ctx_t *app,
                               uint8_t flags,
                               const uint8_t *pcm,
                               size_t pcm_len)
{
    nfa1_header_t hdr = {
        .flags = flags,
        .format = NFA1_FORMAT_PCM16_16K,
        .reserved = 0U,
        .utterance_id = app->current_utterance_id,
        .chunk_seq = app->current_chunk_seq,
        .frame_seq = app->conn_frame_seq++,
    };
    uint8_t buf[NF_MAX_NFA1_FRAME_BYTES];
    size_t packed = nfa1_pack_frame(&hdr, pcm, pcm_len, buf, sizeof(buf));
    if (packed > 0U) {
        (void)nf_uplink_queue_push(&app->uplink_q, buf, packed);
    }
    if (pcm_len > 0U || (flags & NFA1_FLAG_START) != 0U) {
        app->current_chunk_seq++;
    }
}

static void start_new_utterance(nf_app_ctx_t *app, const uint8_t *opt_first_pcm, size_t pcm_len)
{
    app->current_utterance_id = nf_utterance_gen_next(&app->utt_gen);
    app->current_chunk_seq = 0U;
    app->segment_frames_sent = (pcm_len > 0U) ? 1U : 0U;
    app->consecutive_silence_frames = 0U;
    app->utterance_open = true;
    app->pending_finals_count++;

    enqueue_nfa1_frame(app, NFA1_FLAG_START, opt_first_pcm, pcm_len);
}

static void close_current_utterance(nf_app_ctx_t *app, uint8_t end_or_cancel_flag)
{
    if (!app->utterance_open) {
        return;
    }
    enqueue_nfa1_frame(app, end_or_cancel_flag, NULL, 0U);
    app->utterance_open = false;
    app->segment_frames_sent = 0U;
}

static void emit_send_turn(nf_app_ctx_t *app, const char *text)
{
    if (!app->send_data_json || !text || text[0] == '\0') {
        return;
    }
    char json_buf[1280];
    int n = snprintf(json_buf,
                     sizeof(json_buf),
                     "{\"k\":\"frame\",\"frame\":{\"kind\":\"send\",\"text\":\"%s\"}}",
                     text);
    if (n > 0 && (size_t)n < sizeof(json_buf)) {
        app->send_data_json(json_buf, app->user_ctx);
    }
}

void nf_app_init(nf_app_ctx_t *app,
                 uint32_t boot_nonce,
                 nf_send_data_json_cb_t send_data_json,
                 void *user_ctx)
{
    if (!app) {
        return;
    }
    memset(app, 0, sizeof(*app));
    app->state = NF_APP_STATE_IDLE;
    nf_utterance_gen_init(&app->utt_gen, boot_nonce);
    nf_uplink_queue_init(&app->uplink_q);
    nf_asset_stream_init(&app->downlink_stream);
    app->send_data_json = send_data_json;
    app->user_ctx = user_ctx;
    (void)bsp_audio_init(BSP_AUDIO_DEFAULT_MIC_RATE);
}

void nf_app_on_key_event(nf_app_ctx_t *app, bsp_key_event_t ev)
{
    if (!app) {
        return;
    }

    switch (ev) {
    case BSP_KEY_EVENT_BOOT_PRESS_START:
        if (app->state == NF_APP_STATE_PLAYING) {
            /* Barge-in: immediately stop TTS playback, notify daemon, restore 16kHz mic */
            nf_asset_stream_stop(&app->downlink_stream);
            (void)bsp_audio_restore_capture_16k();
            if (app->send_data_json) {
                app->send_data_json("{\"k\":\"frame\",\"frame\":{\"kind\":\"voice_barge_in\"}}",
                                    app->user_ctx);
            }
        } else if (app->state == NF_APP_STATE_MEETING) {
            close_current_utterance(app, NFA1_FLAG_END);
        }

        app->state = NF_APP_STATE_LISTENING;
        app->ptt_accum_text[0] = '\0';
        app->pending_finals_count = 0U;
        start_new_utterance(app, NULL, 0U);
        break;

    case BSP_KEY_EVENT_BOOT_RELEASE:
        if (app->state == NF_APP_STATE_LISTENING) {
            close_current_utterance(app, NFA1_FLAG_END);
            app->state = NF_APP_STATE_WAITING_REPLY;
        }
        break;

    case BSP_KEY_EVENT_PWR_CLICK:
        if (app->state == NF_APP_STATE_LISTENING || app->state == NF_APP_STATE_WAITING_REPLY) {
            /* Cancel current turn: do not send `{kind:"send", text}` */
            if (app->utterance_open) {
                close_current_utterance(app, NFA1_FLAG_CANCEL);
            }
            app->ptt_accum_text[0] = '\0';
            app->pending_finals_count = 0U;
            app->state = NF_APP_STATE_IDLE;
        } else if (app->state == NF_APP_STATE_PLAYING) {
            nf_asset_stream_stop(&app->downlink_stream);
            (void)bsp_audio_restore_capture_16k();
            if (app->send_data_json) {
                app->send_data_json("{\"k\":\"frame\",\"frame\":{\"kind\":\"voice_barge_in\"}}",
                                    app->user_ctx);
            }
            app->state = NF_APP_STATE_IDLE;
        }
        break;

    case BSP_KEY_EVENT_BOOT_DOUBLE_CLICK:
        /* Toggle hands-free Meeting Mode (/meeting) */
        if (app->state == NF_APP_STATE_MEETING) {
            close_current_utterance(app, NFA1_FLAG_END);
            app->state = NF_APP_STATE_IDLE;
        } else if (app->state == NF_APP_STATE_IDLE) {
            app->state = NF_APP_STATE_MEETING;
            app->utterance_open = false;
            app->consecutive_silence_frames = 0U;
            app->pending_finals_count = 0U;
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
        /* Push-to-Talk Mode:
         * Button controls primary start/end so short pauses (<20s) never split the turn early.
         * If continuous speech exceeds 20s (1000 frames) and VAD detects silence,
         * auto-split into a new utterance_id to prevent SenseVoice O(L^2) explosion,
         * and concatenate the resulting speech_final texts locally until button release.
         */
        if (!app->utterance_open) {
            start_new_utterance(app, pcm_640, NF_PCM_BYTES_PER_FRAME);
            return;
        }

        enqueue_nfa1_frame(app, 0U, pcm_640, NF_PCM_BYTES_PER_FRAME);
        app->segment_frames_sent++;

        if (!is_speech && app->segment_frames_sent >= NF_PTT_MAX_FRAMES_PER_SEG) {
            close_current_utterance(app, NFA1_FLAG_END);
            start_new_utterance(app, NULL, 0U);
        }
        return;
    }

    if (app->state == NF_APP_STATE_MEETING) {
        /* Hands-free Meeting Mode:
         * Driven purely by ESP-SR VAD: open utterance on speech onset,
         * close utterance after 800ms (40 frames) of continuous silence.
         */
        if (is_speech) {
            app->consecutive_silence_frames = 0U;
            if (!app->utterance_open) {
                start_new_utterance(app, pcm_640, NF_PCM_BYTES_PER_FRAME);
            } else {
                enqueue_nfa1_frame(app, 0U, pcm_640, NF_PCM_BYTES_PER_FRAME);
                app->segment_frames_sent++;
            }
        } else if (app->utterance_open) {
            app->consecutive_silence_frames++;
            enqueue_nfa1_frame(app, 0U, pcm_640, NF_PCM_BYTES_PER_FRAME);
            app->segment_frames_sent++;
            if (app->consecutive_silence_frames >= NF_MEETING_SILENCE_FRAMES ||
                app->segment_frames_sent >= NF_PTT_MAX_FRAMES_PER_SEG) {
                close_current_utterance(app, NFA1_FLAG_END);
            }
        }
    }
}

void nf_app_on_speech_partial(nf_app_ctx_t *app, uint64_t utterance_id, const char *text)
{
    (void)utterance_id;
    if (!app || !text) {
        return;
    }
    snprintf(app->screen_transcript, sizeof(app->screen_transcript), "%s", text);
}

void nf_app_on_speech_final(nf_app_ctx_t *app, uint64_t utterance_id, const char *text, bool accepted)
{
    (void)utterance_id;
    if (!app) {
        return;
    }

    if (app->pending_finals_count > 0U) {
        app->pending_finals_count--;
    }

    if (accepted && text && text[0] != '\0') {
        snprintf(app->screen_transcript, sizeof(app->screen_transcript), "%s", text);

        if (app->state == NF_APP_STATE_LISTENING || app->state == NF_APP_STATE_WAITING_REPLY) {
            size_t cur_len = strlen(app->ptt_accum_text);
            if (cur_len < sizeof(app->ptt_accum_text) - 1U) {
                snprintf(app->ptt_accum_text + cur_len,
                         sizeof(app->ptt_accum_text) - cur_len,
                         "%s",
                         text);
            }
        }
    }

    /* Mode A turn completion: only send `{kind:"send", text}` when user has released BOOT
     * (`NF_APP_STATE_WAITING_REPLY`) AND all split segments have returned their `speech_final`.
     * In `NF_APP_STATE_MEETING`, we deliberately do NOT send TTS trigger per sentence!
     */
    if (app->state == NF_APP_STATE_WAITING_REPLY && app->pending_finals_count == 0U) {
        if (app->ptt_accum_text[0] != '\0') {
            emit_send_turn(app, app->ptt_accum_text);
            app->ptt_accum_text[0] = '\0';
        } else {
            app->state = NF_APP_STATE_IDLE;
        }
    }
}

void nf_app_on_asset_offer(nf_app_ctx_t *app, const char *asset_id, uint64_t size, bool media_open)
{
    if (!app || !asset_id) {
        return;
    }
    app->state = NF_APP_STATE_PLAYING;
    nf_asset_stream_start(&app->downlink_stream, asset_id, size);

    /* Immediately pipeline up to MAX_IN_FLIGHT (2) x 16KB asset_pull requests */
    char pull_json[256];
    while (nf_asset_stream_next_pull(&app->downlink_stream, media_open, pull_json, sizeof(pull_json))) {
        if (app->send_data_json) {
            app->send_data_json(pull_json, app->user_ctx);
        }
    }
}

void nf_app_on_playback_finished(nf_app_ctx_t *app)
{
    if (!app) {
        return;
    }
    nf_asset_stream_stop(&app->downlink_stream);
    (void)bsp_audio_restore_capture_16k();
    if (app->state == NF_APP_STATE_PLAYING) {
        app->state = NF_APP_STATE_IDLE;
    }
}
