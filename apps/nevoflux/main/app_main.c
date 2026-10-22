#include <stdio.h>
#include <string.h>
#include "nevoflux_app.h"
#include "pendant_wifi.h"

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static nf_app_ctx_t  s_app;
static prov_rpc_ctx_t s_prov_rpc;

static void send_data_channel_wire_json(const char *wire_json, void *user_ctx)
{
    (void)user_ctx;
#if defined(ESP_PLATFORM)
    ESP_LOGI("nevo_ws", "TX data: %s", wire_json);
#else
    (void)wire_json;
#endif
}

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
    (void)cred;
    (void)user_ctx;
    snprintf(s_app.screen_transcript,
             sizeof(s_app.screen_transcript),
             "凭证已更新，正在重连 Relay…");
#if defined(ESP_PLATFORM)
    if (pendant && pendant->has_wifi) {
        (void)pendant_wifi_start(pendant->wifi_ssid, pendant->wifi_pass);
    }
#else
    (void)pendant;
#endif
}

#if defined(ESP_PLATFORM)
/**
 * Core 1 Real-Time Audio Task:
 * Reads 20ms (640B) 16kHz PCM frames from ES7210 I2S RX, runs VAD, and feeds NFA1 queue.
 */
static void audio_core1_task(void *arg)
{
    (void)arg;
    (void)esp_task_wdt_add(NULL);

    int16_t pcm_samples[NF_PCM_SAMPLES_PER_FRAME];
    for (;;) {
        (void)esp_task_wdt_reset();
        if (s_app.state == NF_APP_STATE_LISTENING || s_app.state == NF_APP_STATE_MEETING) {
            size_t read_cnt = 0U;
            if (bsp_audio_read_mic(pcm_samples, NF_PCM_SAMPLES_PER_FRAME, &read_cnt, 25U) &&
                read_cnt == NF_PCM_SAMPLES_PER_FRAME) {
                /* Simple energy + zero-crossing fallback if ESP-SR handle not loaded */
                int32_t energy = 0;
                for (size_t i = 0; i < NF_PCM_SAMPLES_PER_FRAME; ++i) {
                    int32_t v = pcm_samples[i];
                    energy += (v < 0) ? -v : v;
                }
                bool is_speech = (energy / (int32_t)NF_PCM_SAMPLES_PER_FRAME) > 350;
                nf_app_on_mic_frame(&s_app, (const uint8_t *)pcm_samples, is_speech);
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

    uint32_t boot_nonce = 0x13572468U;
#if defined(ESP_PLATFORM)
    boot_nonce = esp_random();
    if (boot_nonce == 0U) {
        boot_nonce = 1U;
    }
#endif
    nf_app_init(&s_app, boot_nonce, send_data_channel_wire_json, NULL);
    prov_rpc_init(&s_prov_rpc, on_prov_prompt, on_prov_applied, NULL);

    pendant_config_t pendant_cfg;
    (void)prov_nvs_load_pendant(&pendant_cfg);

    nf_credentials_t nf_cred;
    if (!prov_nvs_load_nf_cfg(&nf_cred)) {
        snprintf(s_app.screen_transcript,
                 sizeof(s_app.screen_transcript),
                 "未配对，请连接电脑 USB 运行 provision.py 或切回 Launcher 配对");
    }

#if defined(ESP_PLATFORM)
    (void)prov_usb_line_init();
    (void)pendant_wifi_start(pendant_cfg.has_wifi ? pendant_cfg.wifi_ssid : NULL, pendant_cfg.wifi_pass);

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
            nf_app_on_key_event(&s_app, kev);
        }

#if defined(ESP_PLATFORM)
        if (prov_usb_line_poll(line_buf, sizeof(line_buf)) &&
            prov_rpc_handle_line(&s_prov_rpc, line_buf, t_ms, resp_buf, sizeof(resp_buf)) > 0) {
            printf("%s\n", resp_buf);
            fflush(stdout);
            (void)prov_nvs_load_pendant(&pendant_cfg);
            (void)prov_nvs_load_nf_cfg(&nf_cred);
        }
        if (t_ms - last_net_ms >= 500ULL) {
            char net_line[96];
            pendant_wifi_format_line(net_line, sizeof(net_line));
            bsp_display_set_net_info(net_line);
            last_net_ms = t_ms;
        }
        pendant_wifi_status_t wifi_st;
        pendant_wifi_get_status(&wifi_st);
        const bool wifi_up = (wifi_st.state == PENDANT_WIFI_CONNECTED);
#else
        const bool wifi_up = pendant_cfg.has_wifi;
#endif

        bsp_ui_touch_action_t touch = bsp_display_poll_touch();
        if (touch == BSP_UI_TOUCH_SWITCH_SYS) {
            (void)bsp_reboot_to_launcher();
        } else if (touch == BSP_UI_TOUCH_TOGGLE_MEET) {
            nf_app_on_key_event(&s_app, BSP_KEY_EVENT_BOOT_DOUBLE_CLICK);
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
            .relay_connected = nf_cred.is_paired,
            .daemon_online = nf_cred.is_paired,
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
