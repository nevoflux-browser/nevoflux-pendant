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
#else
#define APP_LOCK()   ((void)0)
#define APP_UNLOCK() ((void)0)
#endif

static void on_prov_prompt(const char *device_name, uint32_t timeout_sec, void *user_ctx)
{
    (void)user_ctx;
    snprintf(s_app.screen_transcript,
             sizeof(s_app.screen_transcript),
             "收到 [%s] 配对请求，请 %us 内单击 BOOT 键确认",
             device_name ? device_name : "PC",
             (unsigned)timeout_sec);
}

static void on_prov_applied(const pendant_config_t *pendant,
                            const nf_credentials_t *cred,
                            void *user_ctx)
{
    (void)user_ctx;
    snprintf(s_app.screen_transcript,
             sizeof(s_app.screen_transcript),
             "凭证已更新，正在重连…");
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
    } else if (strcmp(kind, "turn_end") == 0) {
        nf_app_on_turn_end(&s_app);
    } else if (strcmp(kind, "error") == 0) {
        nf_app_on_error(&s_app, jstr(frame, "message"));
    } else if (strcmp(kind, "voice_audio") == 0) {
        const cJSON *seq = cJSON_GetObjectItem(frame, "seq");
        bool play = nf_app_on_voice_audio(&s_app, jstr(frame, "turn_id"),
                                          cJSON_IsNumber(seq) ? (uint32_t)seq->valuedouble : 0U);
        APP_UNLOCK();
        if (play) {
            play_voice_audio(jstr(frame, "wav"));
        }
        cJSON_Delete(root);
        return;
    } else if (strcmp(kind, "voice_done") == 0) {
        nf_app_on_voice_done(&s_app, jstr(frame, "turn_id"));
    } else if (strcmp(kind, "voice_failed") == 0) {
        nf_app_on_voice_failed(&s_app, jstr(frame, "turn_id"), jstr(frame, "message"));
    }
    APP_UNLOCK();
    cJSON_Delete(root);
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
                bool is_speech = (energy / (int32_t)NF_PCM_SAMPLES_PER_FRAME) > 350;
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
    const nf_app_io_t io = {.send_frame = io_send_frame, .stop_playback = io_stop_playback, .ctx = NULL};
    nf_app_init(&s_app, boot_nonce, &io);
    (void)nf_player_init();
#else
    nf_app_init(&s_app, boot_nonce, NULL);
#endif
    prov_rpc_init(&s_prov_rpc, on_prov_prompt, on_prov_applied, NULL);

    pendant_config_t pendant_cfg;
    (void)prov_nvs_load_pendant(&pendant_cfg);

    if (!prov_nvs_load_nf_cfg(&s_cred) || !s_cred.is_paired) {
        snprintf(s_app.screen_transcript,
                 sizeof(s_app.screen_transcript),
                 "未配对，请连接电脑 USB 运行 provision.py 或切回 Launcher 配对");
    } else {
        snprintf(s_app.screen_transcript, sizeof(s_app.screen_transcript), "正在寻找电脑…");
    }

#if defined(ESP_PLATFORM)
    (void)prov_usb_line_init();
    (void)pendant_wifi_start(pendant_cfg.has_wifi ? pendant_cfg.wifi_ssid : NULL, pendant_cfg.wifi_pass);
    (void)nf_link_start(&s_cred);

    /* Pin real-time audio task to Core 1; keep Core 0 for WiFi, TLS WebSockets & UI */
    xTaskCreatePinnedToCore(audio_core1_task, "audio_c1", 6144, NULL, 10, NULL, 1);
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

        /* Whatever the head sent since the last pass (bounded, to keep the UI moving) */
        for (int i = 0; i < 8; ++i) {
            char *msg = nf_link_poll();
            if (!msg) {
                break;
            }
            dispatch(msg);
            nf_link_free(msg);
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
            snprintf(s_app.screen_transcript, sizeof(s_app.screen_transcript),
                     "已连接%s%s\n按住 BOOT 说话", link.session_title[0] ? "：" : "", link.session_title);
        } else if (!online && was_online && s_app.state == NF_APP_STATE_IDLE) {
            snprintf(s_app.screen_transcript, sizeof(s_app.screen_transcript),
                     link.data_up ? "电脑上的 NevoFlux 未在线" : "正在重新连接电脑…");
        } else if (link.auth_revoked) {
            snprintf(s_app.screen_transcript, sizeof(s_app.screen_transcript),
                     "账号授权已失效，请重新运行 provision.py 配对");
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
        const char *link_label = (link.mode == NF_LINK_LAN) ? "局域网" : "Relay";
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

        const char *mode_str = "待机";
        if (s_app.state == NF_APP_STATE_LISTENING) {
            mode_str = "聆听中…";
        } else if (s_app.state == NF_APP_STATE_WAITING_REPLY) {
            mode_str = "思考中…";
        } else if (s_app.state == NF_APP_STATE_PLAYING) {
            mode_str = "播报中…";
        } else if (s_app.state == NF_APP_STATE_MEETING) {
            mode_str = "会议录音中";
        }

        bsp_ui_status_t ui_st = {
            .wifi_connected = wifi_up,
            .relay_connected = link_up,
            .daemon_online = head_online,
            .link_label = link_label,
            .battery_percent = bsp_power_get_battery_percent(NULL),
            .mode_label = mode_str,
            .transcript = s_app.screen_transcript,
        };
        bsp_display_render_nevoflux(&ui_st);

#if defined(ESP_PLATFORM)
        vTaskDelay(pdMS_TO_TICKS(20));
#else
        break;
#endif
    }
}
