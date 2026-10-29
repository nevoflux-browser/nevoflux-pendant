#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nevoflux_app.h"
#include "pendant_wifi.h"

#if defined(ESP_PLATFORM)
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nf_link.h"
#include "nf_player.h"
#include "encoder/impl/esp_opus_enc.h"

static const char *TAG = "nevoflux";
#endif

static nf_app_ctx_t   s_app;
static prov_rpc_ctx_t s_prov_rpc;
static nf_credentials_t s_cred;

#if defined(ESP_PLATFORM)
/* The app state is touched by the main loop and by the mic task */
static SemaphoreHandle_t s_app_lock;
#define APP_LOCK()   xSemaphoreTake(s_app_lock, portMAX_DELAY)
#define APP_UNLOCK() xSemaphoreGive(s_app_lock)

static void io_send_frame(const char *frame_json, void *ctx)
{
    (void)ctx;
    if (!nf_link_send_frame(frame_json)) {
        ESP_LOGW(TAG, "frame not sent (link down)");
    }
}

static void io_stop_playback(void *ctx)
{
    (void)ctx;
    nf_player_stop();
}

/*
 * The microphone goes up as 16 kHz mono Opus, 20 ms packets at 24 kbit/s:
 * about 4 KB/s base64'd instead of 43 KB/s of PCM, which a phone hotspot to
 * the relay could not carry. Runs on the mic task (its stack is sized for it).
 */
static void *s_opus_enc;

static bool opus_enc_open(void)
{
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    cfg.channel = ESP_AUDIO_MONO;
    cfg.bitrate = 24000;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 5;
    cfg.enable_vbr = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_opus_enc) != ESP_AUDIO_ERR_OK) {
        s_opus_enc = NULL;
        ESP_LOGE(TAG, "Opus encoder unavailable; sending raw PCM");
        return false;
    }
    return true;
}

static size_t io_encode_frame(const int16_t *pcm, uint8_t *out, size_t cap, void *ctx)
{
    (void)ctx;
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = NF_PCM_BYTES_PER_FRAME};
    esp_audio_enc_out_frame_t o = {.buffer = out, .len = (uint32_t)cap};
    if (esp_opus_enc_process(s_opus_enc, &in, &o) != ESP_AUDIO_ERR_OK) {
        return 0U;
    }
    return o.encoded_bytes;
}

static void io_encode_reset(void *ctx)
{
    (void)ctx;
    (void)esp_opus_enc_reset(s_opus_enc);
}
#else
#define APP_LOCK()   ((void)0)
#define APP_UNLOCK() ((void)0)
#endif

static void on_prov_prompt(const char *device_name, uint32_t timeout_sec, void *user_ctx)
{
    (void)user_ctx;
    snprintf(s_app.screen_prompt,
             sizeof(s_app.screen_prompt),
             "收到 [%s] 配对请求，请 %us 内单击 BOOT 键确认",
             device_name ? device_name : "PC",
             (unsigned)timeout_sec);
}

static void on_prov_applied(const pendant_config_t *pendant,
                            const nf_credentials_t *cred,
                            void *user_ctx)
{
    (void)user_ctx;
    nf_app_prompt(&s_app, "凭证已更新，正在重连…");
#if defined(ESP_PLATFORM)
    if (pendant && pendant->has_wifi) {
        (void)pendant_wifi_start(pendant->wifi_ssid, pendant->wifi_pass);
    }
    if (cred) {
        s_cred = *cred;
        nf_link_restart(cred);
    }
#else
    (void)pendant;
    (void)cred;
#endif
}

#if defined(ESP_PLATFORM)
static const char *jstr(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* One spoken sentence: base64 WAV -> 16 kHz PCM -> speaker queue. */
static void play_voice_audio(const char *wav_b64)
{
    size_t b64_len = strlen(wav_b64);
    size_t wav_cap = b64_len / 4U * 3U + 3U;
    uint8_t *wav = heap_caps_malloc(wav_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        return;
    }
    bool ok = false;
    size_t wav_len = nf_b64_decode(wav_b64, b64_len, wav, wav_cap, &ok);
    /* At most 1.5x the samples of 16 kHz input (from 24 kHz it is 2/3) */
    size_t pcm_cap = wav_len;
    int16_t *pcm = ok ? heap_caps_malloc(pcm_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (pcm) {
        size_t n = nf_reply_wav_to_16k(wav, wav_len, pcm, pcm_cap);
        if (n > 0U) {
            (void)nf_player_push(pcm, n);
        } else {
            ESP_LOGW(TAG, "voice_audio is not 16-bit PCM WAV; skipped");
        }
        free(pcm);
    }
    free(wav);
}

/* One spoken sentence as Opus (what this pendant asks for: `voice_codec`). */
static void play_voice_opus(const char *opus_b64)
{
    size_t b64_len = strlen(opus_b64);
    size_t cap = b64_len / 4U * 3U + 3U;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }
    bool ok = false;
    size_t len = nf_b64_decode(opus_b64, b64_len, buf, cap, &ok);
    if (!ok) {
        ESP_LOGW(TAG, "voice_audio opus: not base64; skipped");
        free(buf);
        return;
    }
    (void)nf_player_queue_opus(buf, len);
}

/*
 * The attached conversation's mode ("chat" / "agent"), as the head last said in
 * session_state (on attach, and after a set_mode). The mode button flips it.
 */
static char     s_sess_mode[8];
static char     s_mode_req[8];   /* asked for, not yet confirmed */
static uint64_t s_mode_req_ms;
#define NF_MODE_CONFIRM_MS 5000ULL

static const char *mode_label(void)
{
    if (s_mode_req[0]) {
        return "切换中…";
    }
    if (strcmp(s_sess_mode, "agent") == 0) {
        return "智能体模式";
    }
    if (strcmp(s_sess_mode, "chat") == 0) {
        return "对话模式";
    }
    return NULL;
}

/* App lock held. */
static void on_session_state(const char *mode)
{
    if (strcmp(mode, "chat") != 0 && strcmp(mode, "agent") != 0) {
        return;
    }
    snprintf(s_sess_mode, sizeof(s_sess_mode), "%s", mode);
    if (s_mode_req[0]) {
        nf_app_prompt(&s_app, strcmp(mode, s_mode_req) == 0
                                  ? (strcmp(mode, "agent") == 0 ? "已切换到智能体模式" : "已切换到对话模式")
                                  : "模式切换失败");
        s_mode_req[0] = '\0';
    }
}

/* App lock held. The mode button: only between turns, and only with a head. */
static void toggle_session_mode(bool head_online)
{
    if (s_mode_req[0] || s_app.state != NF_APP_STATE_IDLE) {
        return;
    }
    const char *want = strcmp(s_sess_mode, "agent") == 0 ? "chat" : "agent";
    if (!head_online || !nf_link_request_mode(want)) {
        nf_app_prompt(&s_app, "未连接电脑，暂时无法切换模式");
        return;
    }
    snprintf(s_mode_req, sizeof(s_mode_req), "%s", want);
    s_mode_req_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
}

/* App lock held. */
static void check_mode_request(void)
{
    if (s_mode_req[0] && (uint64_t)(esp_timer_get_time() / 1000LL) - s_mode_req_ms >= NF_MODE_CONFIRM_MS) {
        s_mode_req[0] = '\0';
        nf_app_prompt(&s_app, "模式切换失败");
    }
}

/* One decrypted data-channel message from the head. */
static void dispatch(const char *plaintext)
{
    cJSON *root = cJSON_Parse(plaintext);
    if (!root) {
        return;
    }
    const cJSON *frame = cJSON_GetObjectItem(root, "frame");
    const char *kind = frame ? jstr(frame, "kind") : "";

    APP_LOCK();
    if (strcmp(kind, "speech_partial") == 0) {
        nf_app_on_speech_partial(&s_app, jstr(frame, "utterance_id"), jstr(frame, "text"));
    } else if (strcmp(kind, "speech_final") == 0) {
        nf_app_on_speech_final(&s_app, jstr(frame, "utterance_id"), jstr(frame, "text"),
                               cJSON_IsTrue(cJSON_GetObjectItem(frame, "accepted")));
    } else if (strcmp(kind, "speech_error") == 0) {
        nf_app_on_speech_error(&s_app, jstr(frame, "utterance_id"), jstr(frame, "message"));
    } else if (strcmp(kind, "stream_delta") == 0) {
        nf_app_on_stream_delta(&s_app, jstr(frame, "delta"));
    } else if (strcmp(kind, "turn_end") == 0 || strcmp(kind, "stream_end") == 0) {
        nf_app_on_turn_end(&s_app);
    } else if (strcmp(kind, "error") == 0) {
        nf_app_on_error(&s_app, jstr(frame, "message"));
    } else if (strcmp(kind, "voice_audio") == 0) {
        const cJSON *seq = cJSON_GetObjectItem(frame, "seq");
        bool play = nf_app_on_voice_audio(&s_app, jstr(frame, "turn_id"),
                                          cJSON_IsNumber(seq) ? (uint32_t)seq->valuedouble : 0U);
        APP_UNLOCK();
        if (play) {
            if (strcmp(jstr(frame, "codec"), "opus") == 0) {
                play_voice_opus(jstr(frame, "opus"));
            } else {
                play_voice_audio(jstr(frame, "wav"));
            }
        }
        cJSON_Delete(root);
        return;
    } else if (strcmp(kind, "session_state") == 0) {
        on_session_state(jstr(frame, "mode"));
    } else if (strcmp(kind, "voice_done") == 0) {
        nf_app_on_voice_done(&s_app, jstr(frame, "turn_id"));
    } else if (strcmp(kind, "voice_failed") == 0) {
        nf_app_on_voice_failed(&s_app, jstr(frame, "turn_id"), jstr(frame, "message"));
    }
    APP_UNLOCK();
    cJSON_Delete(root);
}

/* 0..100 for the screen's level bars */
static volatile uint8_t s_mic_level;

static uint8_t level_of(int32_t avg_abs)
{
    const int32_t v = (avg_abs - 200) * 100 / 2500; /* speech runs ~500..3000 */
    return (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
}

/**
 * Core 1: 20 ms (640 B) 16 kHz mic frames into the app while it is listening.
 * The mic runs all the time (it shares the bus clock with the speaker), so a
 * frame is read either way and simply not used when nobody is talking.
 */
static void audio_core1_task(void *arg)
{
    (void)arg;
    (void)esp_task_wdt_add(NULL);

    int16_t pcm_samples[NF_PCM_SAMPLES_PER_FRAME];
    for (;;) {
        (void)esp_task_wdt_reset();
        const nf_app_state_t st = s_app.state;
        if (st == NF_APP_STATE_LISTENING || st == NF_APP_STATE_MEETING) {
            size_t read_cnt = 0U;
            if (bsp_audio_read_mic(pcm_samples, NF_PCM_SAMPLES_PER_FRAME, &read_cnt, 25U) &&
                read_cnt == NF_PCM_SAMPLES_PER_FRAME) {
                /* Energy VAD: only decides meeting segments and the >20 s split point */
                int32_t energy = 0;
                for (size_t i = 0; i < NF_PCM_SAMPLES_PER_FRAME; ++i) {
                    int32_t v = pcm_samples[i];
                    energy += (v < 0) ? -v : v;
                }
                const int32_t avg = energy / (int32_t)NF_PCM_SAMPLES_PER_FRAME;
                bool is_speech = avg > 350;
                s_mic_level = level_of(avg);
                APP_LOCK();
                nf_app_on_mic_frame(&s_app, (const uint8_t *)pcm_samples, is_speech);
                APP_UNLOCK();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
#endif

void app_main(void)
{
    /* 1. Hard brick-recovery check: hold BOOT >= 2s at boot -> erase otadata -> Launcher */
    bsp_check_boot_escape_2s();

    (void)prov_nvs_init();
    (void)bsp_display_init();
    (void)bsp_audio_init(BSP_AUDIO_DEFAULT_MIC_RATE);

    uint32_t boot_nonce = 0x13572468U;
#if defined(ESP_PLATFORM)
    boot_nonce = esp_random();
    if (boot_nonce == 0U) {
        boot_nonce = 1U;
    }
    s_app_lock = xSemaphoreCreateMutex();
    const bool opus = opus_enc_open();
    const nf_app_io_t io = {.send_frame = io_send_frame,
                            .stop_playback = io_stop_playback,
                            .encode_frame = opus ? io_encode_frame : NULL,
                            .encode_reset = opus ? io_encode_reset : NULL,
                            .ctx = NULL};
    nf_app_init(&s_app, boot_nonce, &io);
    (void)nf_player_init();
#else
    nf_app_init(&s_app, boot_nonce, NULL);
#endif
    prov_rpc_init(&s_prov_rpc, on_prov_prompt, on_prov_applied, NULL);

    pendant_config_t pendant_cfg;
    (void)prov_nvs_load_pendant(&pendant_cfg);

    if (!prov_nvs_load_nf_cfg(&s_cred) || !s_cred.is_paired) {
        nf_app_prompt(&s_app, "未配对，请连接电脑 USB 运行 provision.py 或切回 Launcher 配对");
    } else {
        nf_app_prompt(&s_app, "正在寻找电脑…");
    }

#if defined(ESP_PLATFORM)
    (void)prov_usb_line_init();
    (void)pendant_wifi_start(pendant_cfg.has_wifi ? pendant_cfg.wifi_ssid : NULL, pendant_cfg.wifi_pass);
    (void)nf_link_start(&s_cred);

    /* Pin real-time audio task to Core 1; keep Core 0 for WiFi, TLS WebSockets & UI.
     * Opus encoding wants a deep stack; it never touches flash, so PSRAM holds it. */
    xTaskCreatePinnedToCoreWithCaps(audio_core1_task, "audio_c1", 32768, NULL, 10, NULL, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    (void)esp_task_wdt_add(NULL);
#endif

    bsp_key_fsm_t key_fsm;
    bsp_key_init(&key_fsm);
#if defined(ESP_PLATFORM)
    char line_buf[1024];
    char resp_buf[256];
    uint64_t last_net_ms = 0ULL;
    bool was_online = false;
#endif

    for (;;) {
#if defined(ESP_PLATFORM)
        (void)esp_task_wdt_reset();
        uint64_t t_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
#else
        uint64_t t_ms = 0ULL;
#endif
        bsp_key_event_t kev = bsp_key_poll(&key_fsm, t_ms);
        if (kev == BSP_KEY_EVENT_BOOT_SINGLE_CLICK) {
            (void)prov_rpc_confirm_boot(&s_prov_rpc, t_ms);
        } else if (kev != BSP_KEY_EVENT_NONE) {
            APP_LOCK();
            nf_app_on_key_event(&s_app, kev);
            APP_UNLOCK();
        }

#if defined(ESP_PLATFORM)
        if (prov_usb_line_poll(line_buf, sizeof(line_buf)) &&
            prov_rpc_handle_line(&s_prov_rpc, line_buf, t_ms, resp_buf, sizeof(resp_buf)) > 0) {
            printf("%s\n", resp_buf);
            fflush(stdout);
            (void)prov_nvs_load_pendant(&pendant_cfg);
        }

        /* Everything the head sent since the last pass, then one redraw below: a
         * reply streams a delta per token, and redrawing per delta could not keep
         * up. The time budget keeps keys and the watchdog serviced in a flood. */
        for (uint64_t start = t_ms;;) {
            char *msg = nf_link_poll();
            if (!msg) {
                break;
            }
            dispatch(msg);
            nf_link_free(msg);
            if ((uint64_t)(esp_timer_get_time() / 1000LL) - start >= 40ULL) {
                break;
            }
        }
        if (nf_player_take_drained()) {
            APP_LOCK();
            nf_app_on_playback_drained(&s_app);
            APP_UNLOCK();
        }

        nf_link_status_t link;
        nf_link_get_status(&link);
        const bool online = link.data_up && link.head_online && link.attached;
        APP_LOCK();
        nf_app_set_link(&s_app, online);
        nf_app_tick(&s_app, t_ms);
        if (online && !was_online && s_app.state == NF_APP_STATE_IDLE) {
            nf_app_prompt(&s_app, NULL); /* the default hint: connected, hold BOOT */
        } else if (!online && was_online && s_app.state == NF_APP_STATE_IDLE) {
            nf_app_prompt(&s_app, link.data_up ? "电脑上的 NevoFlux 未在线" : "正在重新连接电脑…");
        } else if (link.auth_revoked) {
            nf_app_prompt(&s_app, "账号授权已失效，请重新运行 provision.py 配对");
        }
        APP_UNLOCK();
        was_online = online;

        if (t_ms - last_net_ms >= 500ULL) {
            char net_line[96];
            pendant_wifi_format_line(net_line, sizeof(net_line));
            bsp_display_set_net_info(net_line);
            last_net_ms = t_ms;
        }
        pendant_wifi_status_t wifi_st;
        pendant_wifi_get_status(&wifi_st);
        const bool wifi_up = (wifi_st.state == PENDANT_WIFI_CONNECTED);
        const bool link_up = link.data_up;
        const bool head_online = online;
        const char *link_label = link.on_ble ? "蓝牙" : (link.mode == NF_LINK_LAN) ? "局域网" : "Relay";
#else
        const bool wifi_up = pendant_cfg.has_wifi;
        const bool link_up = s_cred.is_paired;
        const bool head_online = s_cred.is_paired;
        const char *link_label = NULL;
#endif

        bsp_ui_touch_action_t touch = bsp_display_poll_touch();
        if (touch == BSP_UI_TOUCH_SWITCH_SYS) {
            (void)bsp_reboot_to_launcher();
        } else if (touch == BSP_UI_TOUCH_TOGGLE_MEET) {
            APP_LOCK();
            nf_app_on_key_event(&s_app, BSP_KEY_EVENT_BOOT_DOUBLE_CLICK);
            APP_UNLOCK();
        }
#if defined(ESP_PLATFORM)
        APP_LOCK();
        if (touch == BSP_UI_TOUCH_TOGGLE_MODE) {
            toggle_session_mode(link_up && head_online);
        }
        check_mode_request();
        APP_UNLOCK();
#endif

        const char *mode_str = "待机";
        bsp_ui_activity_t activity = BSP_UI_ACT_IDLE;
        uint8_t level = 0U;
        if (s_app.state == NF_APP_STATE_LISTENING) {
            mode_str = "聆听中…";
            activity = BSP_UI_ACT_LISTENING;
        } else if (s_app.state == NF_APP_STATE_WAITING_REPLY) {
            mode_str = "思考中…";
            activity = BSP_UI_ACT_THINKING;
        } else if (s_app.state == NF_APP_STATE_PLAYING) {
            mode_str = "回复中…";
            activity = BSP_UI_ACT_SPEAKING;
        } else if (s_app.state == NF_APP_STATE_MEETING) {
            mode_str = "会议录音中";
            activity = BSP_UI_ACT_MEETING;
        }
#if defined(ESP_PLATFORM)
        level = (activity == BSP_UI_ACT_SPEAKING) ? nf_player_level() : s_mic_level;
#endif
        /* The conversation itself is not shown: only prompts made of the UI's strings */
        const char *prompt = s_app.screen_prompt;
        if (!prompt[0]) {
            prompt = head_online ? "已连接电脑\n按住 BOOT 说话" : "正在寻找电脑…";
        }

        bsp_ui_status_t ui_st = {
            .wifi_connected = wifi_up,
            .relay_connected = link_up,
            .daemon_online = head_online,
            .link_label = link_label,
            .battery_percent = bsp_power_get_battery_percent(NULL),
            .mode_label = mode_str,
            .transcript = prompt,
            .activity = activity,
            .level = level,
        };
#if defined(ESP_PLATFORM)
        ui_st.session_mode_label = mode_label();
#endif
        bsp_display_render_nevoflux(&ui_st);

#if defined(ESP_PLATFORM)
        vTaskDelay(pdMS_TO_TICKS(20));
#else
        break;
#endif
    }
}
