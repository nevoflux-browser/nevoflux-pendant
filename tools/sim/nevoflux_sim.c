/**
 * NevoFlux Voice Pendant 1.43" (466x466) Circular AMOLED UI & Firmware Simulator
 *
 * Links the actual C firmware components (`nf_proto`, `prov_nvs`, `bsp_143c`,
 * `apps/nevoflux`) with a pixel-accurate 466x466 circular renderer (Cairo/PangoCairo)
 * and an optional interactive SDL2 window.
 */

#include <cairo/cairo.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_143c.h"
#include "muse_oauth_device.h"
#include "nevoflux_app.h"
#include "nf_proto.h"
#include "prov_nvs.h"

#ifdef USE_SDL2
#include <SDL2/SDL.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef enum {
    SIM_PARTITION_LAUNCHER = 0, /* factory: Launcher */
    SIM_PARTITION_NEVOFLUX = 1, /* ota_1  : NevoFlux */
    SIM_PARTITION_MUSE     = 2, /* ota_0  : Muse     */
} sim_partition_t;

typedef struct {
    sim_partition_t   active_partition;
    pendant_config_t  pendant_cfg;
    nf_credentials_t  nf_cred;
    prov_rpc_ctx_t    prov_rpc;
    nf_app_ctx_t      nevo_app;
    muse_device_ctx_t muse_app;
    bsp_key_fsm_t     key_fsm;

    uint64_t          now_ms;
    bool              boot_held;
    bool              daemon_online;
    char              launcher_prompt[160];
    char              last_tx_json[512];
    uint32_t          tx_json_count;
} sim_context_t;

static sim_context_t g_sim;

static void start_muse_rfc8628_flow(void)
{
    const char *code_resp =
        "{"
        "\"device_code\":\"muse_dev_code_889900\","
        "\"user_code\":\"ABC-123\","
        "\"verification_uri\":\"muse.ai/link\","
        "\"verification_uri_complete\":\"https://muse.ai/link?code=ABC-123\","
        "\"expires_in\":900,"
        "\"interval\":5"
        "}";
    (void)muse_oauth_on_device_code_resp(&g_sim.muse_app, code_resp, g_sim.now_ms);
}

void sim_muse_complete_oauth(void)
{
    g_sim.active_partition = SIM_PARTITION_MUSE;
    if (g_sim.muse_app.auth_state != MUSE_AUTH_WAIT_USER_VERIFY) {
        start_muse_rfc8628_flow();
    }
    const char *tok_resp =
        "{"
        "\"access_token\":\"mgst_oauth2_device_flow_token_live\","
        "\"refresh_token\":\"mgrt_refresh_token_live\","
        "\"device_id\":\"MuseGadget-143C\""
        "}";
    (void)muse_oauth_on_token_poll_resp(&g_sim.muse_app, tok_resp, g_sim.now_ms);
    snprintf(g_sim.muse_app.last_transcript,
             sizeof(g_sim.muse_app.last_transcript),
             "OAuth 2.0 (RFC 8628) 授权成功！长效凭证已自动存入 NVS (muse_cfg)。\n按住右侧 BOOT 键即可与 Muse 官方语音助手对话。");
}

static void on_sim_send_data_json(const char *wire_json, void *user_ctx)
{
    (void)user_ctx;
    snprintf(g_sim.last_tx_json, sizeof(g_sim.last_tx_json), "%s", wire_json);
    g_sim.tx_json_count++;
}

static void on_sim_prov_prompt(const char *device_name, uint32_t timeout_sec, void *user_ctx)
{
    (void)user_ctx;
    snprintf(g_sim.launcher_prompt,
             sizeof(g_sim.launcher_prompt),
             "收到 [%s] 配对请求，请 %us 内单击右侧 BOOT 键确认",
             device_name ? device_name : "PC",
             (unsigned)timeout_sec);
    snprintf(g_sim.nevo_app.screen_transcript,
             sizeof(g_sim.nevo_app.screen_transcript),
             "收到 [%s] 配对请求\n请在 %us 内单击 BOOT 键确认落盘",
             device_name ? device_name : "PC",
             (unsigned)timeout_sec);
}

static void on_sim_prov_applied(const pendant_config_t *pendant,
                                const nf_credentials_t *cred,
                                void *user_ctx)
{
    (void)user_ctx;
    if (pendant) {
        g_sim.pendant_cfg = *pendant;
    }
    if (cred) {
        g_sim.nf_cred = *cred;
    }
    g_sim.daemon_online = true;
    snprintf(g_sim.launcher_prompt,
             sizeof(g_sim.launcher_prompt),
             "配网与独立凭证已写入 NVS！点击卡片启动系统");
    snprintf(g_sim.nevo_app.screen_transcript,
             sizeof(g_sim.nevo_app.screen_transcript),
             "凭证注入成功，已连接 Relay 并绑定默认会话！");
}

void sim_init(int initial_partition)
{
    memset(&g_sim, 0, sizeof(g_sim));
    (void)prov_nvs_init();
    (void)bsp_display_init();

    /* Seed default demo WiFi & pairing state so simulator looks realistic out of the box */
    snprintf(g_sim.pendant_cfg.wifi_ssid, sizeof(g_sim.pendant_cfg.wifi_ssid), "NevoFlux-5G");
    snprintf(g_sim.pendant_cfg.wifi_pass, sizeof(g_sim.pendant_cfg.wifi_pass), "private-voice");
    g_sim.pendant_cfg.last_system = (initial_partition == 2) ? PENDANT_SYS_MUSE : PENDANT_SYS_NEVOFLUX;
    g_sim.pendant_cfg.has_wifi = true;
    (void)prov_nvs_save_pendant(&g_sim.pendant_cfg);

    snprintf(g_sim.nf_cred.relay_url, sizeof(g_sim.nf_cred.relay_url), "wss://relay.nevoflux.app");
    snprintf(g_sim.nf_cred.account_token, sizeof(g_sim.nf_cred.account_token), "sess_tok_7d_demo");
    snprintf(g_sim.nf_cred.data_channel_id, sizeof(g_sim.nf_cred.data_channel_id), "a1b2c3d4e5f60718a1b2c3d4e5f60718");
    g_sim.nf_cred.is_paired = true;
    (void)prov_nvs_save_nf_cfg(&g_sim.nf_cred);

    g_sim.active_partition = (initial_partition == 0) ? SIM_PARTITION_LAUNCHER
                           : (initial_partition == 2) ? SIM_PARTITION_MUSE
                                                      : SIM_PARTITION_NEVOFLUX;
    g_sim.daemon_online = true;
    g_sim.now_ms = 1000ULL;

    prov_rpc_init(&g_sim.prov_rpc, on_sim_prov_prompt, on_sim_prov_applied, NULL);
    nf_app_init(&g_sim.nevo_app, 0x20261017U, on_sim_send_data_json, NULL);
    muse_device_init(&g_sim.muse_app);
    /* Start RFC 8628 Device Authorization Grant when Muse has no stored token in NVS */
    if (g_sim.muse_app.auth_state == MUSE_AUTH_UNPAIRED) {
        start_muse_rfc8628_flow();
    }
    bsp_key_init(&g_sim.key_fsm);

    snprintf(g_sim.launcher_prompt,
             sizeof(g_sim.launcher_prompt),
             "点选卡片进入系统，或通过 USB/BLE 注入凭证");
    snprintf(g_sim.nevo_app.screen_transcript,
             sizeof(g_sim.nevo_app.screen_transcript),
             "按住右侧 BOOT 键说话，或双击 BOOT 开启会议实时转写。");
}

void sim_step_ms(uint32_t delta_ms)
{
    g_sim.now_ms += (uint64_t)delta_ms;
    bsp_key_event_t kev = bsp_key_Step(&g_sim.key_fsm, g_sim.boot_held, false, g_sim.now_ms);
    if (kev == BSP_KEY_EVENT_BOOT_SINGLE_CLICK) {
        if (prov_rpc_confirm_boot(&g_sim.prov_rpc, g_sim.now_ms)) {
            snprintf(g_sim.launcher_prompt, sizeof(g_sim.launcher_prompt), "已按 BOOT 键确认！允许写入 NVS");
            snprintf(g_sim.nevo_app.screen_transcript,
                     sizeof(g_sim.nevo_app.screen_transcript),
                     "已按 BOOT 键确认！等待电脑端写入凭证…");
        }
    } else if (kev != BSP_KEY_EVENT_NONE && g_sim.active_partition == SIM_PARTITION_NEVOFLUX) {
        nf_app_on_key_event(&g_sim.nevo_app, kev);
        if (g_sim.nevo_app.state == NF_APP_STATE_LISTENING) {
            snprintf(g_sim.nevo_app.screen_transcript,
                     sizeof(g_sim.nevo_app.screen_transcript),
                     "正在聆听…（16kHz PCM16 → NFA1 24B 帧上行中）");
        }
    } else if (kev != BSP_KEY_EVENT_NONE && g_sim.active_partition == SIM_PARTITION_MUSE &&
               g_sim.muse_app.auth_state == MUSE_AUTH_AUTHORIZED) {
        if (kev == BSP_KEY_EVENT_BOOT_PRESS_START) {
            g_sim.muse_app.voice_state = MUSE_VOICE_LISTENING;
            snprintf(g_sim.muse_app.last_transcript,
                     sizeof(g_sim.muse_app.last_transcript),
                     "Muse 正在聆听… (muse-gadget-sdk 音频流上行中)");
        } else if (kev == BSP_KEY_EVENT_BOOT_RELEASE) {
            g_sim.muse_app.voice_state = MUSE_VOICE_SPEAKING;
            snprintf(g_sim.muse_app.last_transcript,
                     sizeof(g_sim.muse_app.last_transcript),
                     "Muse: 已通过官方 Muse Gadget SDK 收到您的语音指令！");
        }
    }

    /* Feed simulated 20ms mic frames when in LISTENING or MEETING mode */
    if (g_sim.active_partition == SIM_PARTITION_NEVOFLUX &&
        (g_sim.nevo_app.state == NF_APP_STATE_LISTENING ||
         g_sim.nevo_app.state == NF_APP_STATE_MEETING)) {
        uint8_t pcm[NF_PCM_BYTES_PER_FRAME];
        memset(pcm, 0x18, sizeof(pcm));
        nf_app_on_mic_frame(&g_sim.nevo_app, pcm, true);
    }
}

void sim_set_boot_button(bool pressed)
{
    g_sim.boot_held = pressed;
    sim_step_ms(20);
}

void sim_trigger_single_click_boot(void)
{
    g_sim.boot_held = true;
    sim_step_ms(60);
    g_sim.boot_held = false;
    sim_step_ms(400);
}

void sim_trigger_double_click_boot(void)
{
    if (g_sim.active_partition == SIM_PARTITION_NEVOFLUX) {
        nf_app_on_key_event(&g_sim.nevo_app, BSP_KEY_EVENT_BOOT_DOUBLE_CLICK);
        if (g_sim.nevo_app.state == NF_APP_STATE_MEETING) {
            snprintf(g_sim.nevo_app.screen_transcript,
                     sizeof(g_sim.nevo_app.screen_transcript),
                     "【会议模式 /meeting】\n免长按持续录音中，ESP-SR VAD 自动逐句切分，静音不触发 TTS 插话。");
        } else {
            snprintf(g_sim.nevo_app.screen_transcript,
                     sizeof(g_sim.nevo_app.screen_transcript),
                     "会议录音已结束，正在由 /meeting pack 生成结构化纪要并存入 GBrain…");
        }
    }
}

void sim_trigger_pwr_click(void)
{
    if (g_sim.active_partition == SIM_PARTITION_NEVOFLUX) {
        nf_app_on_key_event(&g_sim.nevo_app, BSP_KEY_EVENT_PWR_CLICK);
        snprintf(g_sim.nevo_app.screen_transcript,
                 sizeof(g_sim.nevo_app.screen_transcript),
                 "已按 PWR 键取消当前语音轮次。");
    } else if (g_sim.active_partition == SIM_PARTITION_MUSE) {
        g_sim.active_partition = SIM_PARTITION_LAUNCHER;
    }
}

void sim_touch_xy(int x, int y)
{
    /* Check circular bounds (radius 233 around center 233, 233) */
    int dx = x - 233;
    int dy = y - 233;
    if (dx * dx + dy * dy > 233 * 233) {
        return;
    }

    if (g_sim.active_partition == SIM_PARTITION_LAUNCHER) {
        /* Left card: Muse (x: 45..223, y: 140..310) */
        if (x >= 45 && x <= 223 && y >= 140 && y <= 310) {
            g_sim.pendant_cfg.last_system = PENDANT_SYS_MUSE;
            (void)prov_nvs_save_pendant(&g_sim.pendant_cfg);
            g_sim.active_partition = SIM_PARTITION_MUSE;
            if (g_sim.muse_app.auth_state == MUSE_AUTH_UNPAIRED) {
                start_muse_rfc8628_flow();
            }
            return;
        }
        /* Right card: NevoFlux (x: 243..421, y: 140..310) */
        if (x >= 243 && x <= 421 && y >= 140 && y <= 310) {
            g_sim.pendant_cfg.last_system = PENDANT_SYS_NEVOFLUX;
            (void)prov_nvs_save_pendant(&g_sim.pendant_cfg);
            g_sim.active_partition = SIM_PARTITION_NEVOFLUX;
            return;
        }
        return;
    }

    if (g_sim.active_partition == SIM_PARTITION_MUSE) {
        /* Bottom buttons in Muse:
         * Left : [ 模拟完成授权 ] or [ 重新配对 ] (x: 85..225, y: 360..412)
         * Right: [ 切换系统 ] (x: 241..381, y: 360..412)
         */
        if (y >= 360 && y <= 412) {
            if (x >= 85 && x <= 225) {
                if (g_sim.muse_app.auth_state != MUSE_AUTH_AUTHORIZED) {
                    sim_muse_complete_oauth();
                } else {
                    (void)muse_device_unpair(&g_sim.muse_app);
                    start_muse_rfc8628_flow();
                }
            } else if (x >= 241 && x <= 381) {
                g_sim.active_partition = SIM_PARTITION_LAUNCHER;
            }
        }
        return;
    }

    /* SIM_PARTITION_NEVOFLUX bottom touch buttons:
     * Left : [ 会议模式 ] (x: 85..225, y: 360..412)
     * Right: [ 切换系统 ] (x: 241..381, y: 360..412)
     */
    if (y >= 360 && y <= 412) {
        if (x >= 85 && x <= 225) {
            sim_trigger_double_click_boot();
        } else if (x >= 241 && x <= 381) {
            /* Erase otadata and reboot into factory Launcher */
            (void)bsp_reboot_to_launcher();
            g_sim.active_partition = SIM_PARTITION_LAUNCHER;
        }
    }
}

void sim_inject_speech_partial(const char *text)
{
    g_sim.active_partition = SIM_PARTITION_NEVOFLUX;
    nf_app_on_speech_partial(&g_sim.nevo_app, g_sim.nevo_app.current_utterance_id, text);
}

void sim_inject_speech_final(const char *text, bool accepted)
{
    g_sim.active_partition = SIM_PARTITION_NEVOFLUX;
    nf_app_on_speech_final(&g_sim.nevo_app, g_sim.nevo_app.current_utterance_id, text, accepted);
}

void sim_inject_tts_asset(const char *reply_text, uint32_t sample_rate_hz)
{
    g_sim.active_partition = SIM_PARTITION_NEVOFLUX;
    if (reply_text && reply_text[0] != '\0') {
        snprintf(g_sim.nevo_app.screen_transcript,
                 sizeof(g_sim.nevo_app.screen_transcript),
                 "%s",
                 reply_text);
    }
    nf_app_on_asset_offer(&g_sim.nevo_app, "tts_kokoro_turn_01", 48000ULL, true);

    /* Simulate first NFM1 chunk with WAV header to trigger ES8311 sample rate switch */
    uint8_t wav_hdr[64] = {
        'R', 'I', 'F', 'F', 0x24, 0x80, 0x00, 0x00, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        (uint8_t)(sample_rate_hz & 0xFFU),
        (uint8_t)((sample_rate_hz >> 8) & 0xFFU),
        (uint8_t)((sample_rate_hz >> 16) & 0xFFU),
        (uint8_t)((sample_rate_hz >> 24) & 0xFFU),
        0x80, 0xBB, 0x00, 0x00, 0x02, 0x00, 0x10, 0x00,
        'd', 'a', 't', 'a', 0x00, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    nfm1_frame_t frame = {
        .has_seq = false,
        .id = "tts_kokoro_turn_01",
        .offset = 0ULL,
        .flags = NFM1_FLAG_NO_SEQ,
        .eof = false,
        .payload = wav_hdr,
        .payload_len = 48U,
    };
    const uint8_t *pcm_ptr = NULL;
    size_t pcm_len = 0U;
    (void)nf_asset_stream_on_nfm1(&g_sim.nevo_app.downlink_stream, &frame, &pcm_ptr, &pcm_len);
}

int sim_inject_prov_line(const char *json_line, char *out_resp, size_t resp_cap)
{
    return prov_rpc_handle_line(&g_sim.prov_rpc, json_line, g_sim.now_ms, out_resp, resp_cap);
}

/* =========================================================================
 * Cairo / Pango 466x466 Circular AMOLED (SH8601) Renderer
 * ========================================================================= */

static void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

static void draw_pango_text(cairo_t *cr,
                            const char *text,
                            const char *font_desc_str,
                            double x,
                            double y,
                            double width,
                            PangoAlignment align,
                            double r,
                            double g,
                            double b)
{
    if (!text || text[0] == '\0') {
        return;
    }
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(font_desc_str);
    pango_layout_set_font_description(layout, desc);
    if (width > 0.0) {
        pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(layout, align);
    }
    pango_layout_set_text(layout, text, -1);

    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

static void draw_qr_code_box(cairo_t *cr, double bx, double by, double size, const char *seed_str)
{
    /* White quiet zone rounded card */
    draw_rounded_rect(cr, bx, by, size, size, 12.0);
    cairo_set_source_rgb(cr, 0.98, 0.99, 1.0);
    cairo_fill(cr);

    const int N = 21;
    double pad = 10.0;
    double cell = (size - 2.0 * pad) / (double)N;
    uint32_t h = 2166136261U;
    if (seed_str) {
        for (const char *p = seed_str; *p; ++p) {
            h = (h ^ (uint8_t)(*p)) * 16777619U;
        }
    }

    cairo_set_source_rgb(cr, 0.06, 0.08, 0.14);
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            bool finder = ((r < 7 && c < 7) || (r < 7 && c >= N - 7) || (r >= N - 7 && c < 7));
            bool on = false;
            if (finder) {
                int lr = (r >= N - 7) ? (r - (N - 7)) : r;
                int lc = (c >= N - 7) ? (c - (N - 7)) : c;
                if (lr == 0 || lr == 6 || lc == 0 || lc == 6 ||
                    (lr >= 2 && lr <= 4 && lc >= 2 && lc <= 4)) {
                    on = true;
                }
            } else if ((r == 6 || c == 6)) {
                on = ((r + c) % 2 == 0);
            } else {
                uint32_t bit = ((uint32_t)(r * 31 + c * 17) ^ (h >> ((r + c) % 16))) & 1U;
                on = (bit != 0U);
            }
            if (on) {
                cairo_rectangle(cr, bx + pad + c * cell, by + pad + r * cell, cell + 0.3, cell + 0.3);
                cairo_fill(cr);
            }
        }
    }
}

static void render_launcher_screen(cairo_t *cr)
{
    /* Top status header inside circular safe zone */
    char top_bar[128];
    snprintf(top_bar,
             sizeof(top_bar),
             "WiFi: %s   |   %s",
             g_sim.pendant_cfg.has_wifi ? g_sim.pendant_cfg.wifi_ssid : "未配网",
             g_sim.nf_cred.is_paired ? "已配对" : "未配对");
    draw_pango_text(cr, top_bar, "Sans Bold 13", 53, 52, 360, PANGO_ALIGN_CENTER, 0.58, 0.64, 0.72);

    draw_pango_text(cr, "NevoFlux Pendant Launcher", "Sans Bold 17", 43, 88, 380, PANGO_ALIGN_CENTER, 0.95, 0.97, 1.0);
    draw_pango_text(cr, "factory (0x20000) · 双系统引导与统一配网", "Sans 11", 43, 114, 380, PANGO_ALIGN_CENTER, 0.45, 0.52, 0.62);

    /* Left Card: ota_0 (Muse Official Gadget SDK) */
    bool muse_sel = (g_sim.pendant_cfg.last_system == PENDANT_SYS_MUSE);
    draw_rounded_rect(cr, 48, 148, 174, 158, 20);
    cairo_set_source_rgba(cr, muse_sel ? 0.16 : 0.08, muse_sel ? 0.14 : 0.10, muse_sel ? 0.28 : 0.14, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, muse_sel ? 3.0 : 1.5);
    cairo_set_source_rgba(cr, muse_sel ? 0.66 : 0.22, muse_sel ? 0.45 : 0.26, muse_sel ? 0.98 : 0.34, 1.0);
    cairo_stroke(cr);

    draw_pango_text(cr, "ota_0 · 3.0MB", "Sans Bold 11", 58, 166, 154, PANGO_ALIGN_CENTER, 0.68, 0.55, 0.95);
    draw_pango_text(cr, "Muse", "Sans Bold 24", 58, 196, 154, PANGO_ALIGN_CENTER, 0.96, 0.96, 1.0);
    draw_pango_text(cr, "官方 Gadget SDK\nRFC 8628 设备授权", "Sans 12", 58, 238, 154, PANGO_ALIGN_CENTER, 0.65, 0.70, 0.78);

    /* Right Card: ota_1 (NevoFlux) */
    bool nevo_sel = (g_sim.pendant_cfg.last_system == PENDANT_SYS_NEVOFLUX);
    draw_rounded_rect(cr, 244, 148, 174, 158, 20);
    cairo_set_source_rgba(cr, nevo_sel ? 0.06 : 0.08, nevo_sel ? 0.22 : 0.10, nevo_sel ? 0.24 : 0.14, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, nevo_sel ? 3.0 : 1.5);
    cairo_set_source_rgba(cr, nevo_sel ? 0.15 : 0.22, nevo_sel ? 0.85 : 0.26, nevo_sel ? 0.78 : 0.34, 1.0);
    cairo_stroke(cr);

    draw_pango_text(cr, "ota_1 · 2.5MB", "Sans Bold 11", 254, 166, 154, PANGO_ALIGN_CENTER, 0.22, 0.88, 0.80);
    draw_pango_text(cr, "NevoFlux", "Sans Bold 24", 254, 196, 154, PANGO_ALIGN_CENTER, 0.96, 0.99, 1.0);
    draw_pango_text(cr, "私域语音入口\n直连 Relay → 本机", "Sans 12", 254, 238, 154, PANGO_ALIGN_CENTER, 0.68, 0.82, 0.84);

    /* Bottom status / pairing confirmation banner */
    bool waiting_boot = (g_sim.prov_rpc.state == PROV_STATE_WAIT_CONFIRM);
    draw_rounded_rect(cr, 68, 326, 330, 64, 16);
    if (waiting_boot) {
        cairo_set_source_rgba(cr, 0.35, 0.18, 0.04, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.98, 0.62, 0.15, 1.0);
        cairo_set_line_width(cr, 2.0);
        cairo_stroke(cr);
        draw_pango_text(cr, g_sim.launcher_prompt, "Sans Bold 12", 78, 338, 310, PANGO_ALIGN_CENTER, 1.0, 0.82, 0.45);
    } else {
        cairo_set_source_rgba(cr, 0.09, 0.12, 0.17, 0.9);
        cairo_fill(cr);
        draw_pango_text(cr, g_sim.launcher_prompt, "Sans 12", 78, 344, 310, PANGO_ALIGN_CENTER, 0.62, 0.70, 0.80);
    }
}

static void render_muse_screen(cairo_t *cr)
{
    /* Outer purple/blue glowing ring for Muse Gadget SDK */
    cairo_new_sub_path(cr);
    cairo_set_line_width(cr, 4.0);
    cairo_set_source_rgba(cr, 0.66, 0.45, 0.98, 0.55);
    cairo_arc(cr, 233.0, 233.0, 224.0, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    draw_pango_text(cr,
                    "ota_0 · Muse Gadget SDK (官方 ESP32 SDK)",
                    "Sans Bold 11",
                    58,
                    54,
                    350,
                    PANGO_ALIGN_CENTER,
                    0.76,
                    0.62,
                    0.98);

    if (g_sim.muse_app.auth_state != MUSE_AUTH_AUTHORIZED) {
        /* OAuth 2.0 Device Authorization Grant (RFC 8628) Screen */
        draw_rounded_rect(cr, 78, 82, 310, 36, 18);
        cairo_set_source_rgba(cr, 0.20, 0.14, 0.34, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.6);
        cairo_set_source_rgba(cr, 0.68, 0.48, 0.98, 0.9);
        cairo_stroke(cr);
        draw_pango_text(cr,
                        "OAuth 2.0 设备端授权码模式 (RFC 8628)",
                        "Sans Bold 11",
                        83,
                        92,
                        300,
                        PANGO_ALIGN_CENTER,
                        0.88,
                        0.78,
                        1.0);

        /* Left: 148x148 Scannable QR Code for verification_uri_complete */
        draw_qr_code_box(cr, 48, 140, 148, g_sim.muse_app.verification_uri_complete);

        /* Right: Short code card ("请访问 muse.ai/link 并输入代码 ABC-123") */
        draw_rounded_rect(cr, 210, 140, 210, 148, 18);
        cairo_set_source_rgba(cr, 0.08, 0.11, 0.17, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.4);
        cairo_set_source_rgba(cr, 0.28, 0.34, 0.48, 1.0);
        cairo_stroke(cr);

        draw_pango_text(cr, "手机扫码或浏览器访问", "Sans 11", 218, 152, 194, PANGO_ALIGN_CENTER, 0.64, 0.72, 0.82);
        draw_pango_text(cr,
                        g_sim.muse_app.verification_uri[0] ? g_sim.muse_app.verification_uri : "muse.ai/link",
                        "Sans Bold 15",
                        218,
                        172,
                        194,
                        PANGO_ALIGN_CENTER,
                        0.38,
                        0.85,
                        0.98);
        draw_pango_text(cr, "输入设备授权短码：", "Sans 11", 218, 200, 194, PANGO_ALIGN_CENTER, 0.64, 0.72, 0.82);

        /* Highlighted User Code Badge: ABC-123 */
        draw_rounded_rect(cr, 232, 222, 166, 48, 12);
        cairo_set_source_rgba(cr, 0.18, 0.14, 0.32, 1.0);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.8);
        cairo_set_source_rgba(cr, 0.75, 0.55, 1.0, 1.0);
        cairo_stroke(cr);
        draw_pango_text(cr,
                        g_sim.muse_app.user_code[0] ? g_sim.muse_app.user_code : "ABC-123",
                        "Monospace Bold 22",
                        236,
                        232,
                        158,
                        PANGO_ALIGN_CENTER,
                        0.98,
                        0.92,
                        0.42);

        char poll_hint[128];
        snprintf(poll_hint,
                 sizeof(poll_hint),
                 "ESP32 后台轮询认证接口中 (interval=%us)… 授权后自动存入 NVS",
                 (unsigned)g_sim.muse_app.poll_interval_sec);
        draw_pango_text(cr, poll_hint, "Sans 11", 56, 306, 354, PANGO_ALIGN_CENTER, 0.58, 0.68, 0.80);

        /* Bottom buttons: [ 模拟完成授权 ] | [ 切换系统 ] */
        draw_rounded_rect(cr, 85, 360, 140, 48, 24);
        cairo_set_source_rgba(cr, 0.24, 0.16, 0.42, 1.0);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.6);
        cairo_set_source_rgba(cr, 0.72, 0.52, 0.98, 1.0);
        cairo_stroke(cr);
        draw_pango_text(cr, "模拟完成授权", "Sans Bold 13", 90, 374, 130, PANGO_ALIGN_CENTER, 0.95, 0.92, 1.0);
    } else {
        /* Authorized Muse AI Voice Assistant Screen */
        draw_rounded_rect(cr, 88, 82, 290, 38, 19);
        cairo_set_source_rgba(cr, 0.12, 0.26, 0.22, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.8);
        cairo_set_source_rgba(cr, 0.22, 0.92, 0.65, 0.9);
        cairo_stroke(cr);
        draw_pango_text(cr,
                        "● Muse 已授权连接 (NVS: muse_cfg)",
                        "Sans Bold 13",
                        93,
                        91,
                        280,
                        PANGO_ALIGN_CENTER,
                        0.28,
                        0.95,
                        0.68);

        draw_rounded_rect(cr, 38, 132, 390, 210, 22);
        cairo_set_source_rgba(cr, 0.07, 0.08, 0.14, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.2);
        cairo_set_source_rgba(cr, 0.26, 0.22, 0.42, 1.0);
        cairo_stroke(cr);

        draw_pango_text(cr,
                        g_sim.muse_app.last_transcript[0]
                            ? g_sim.muse_app.last_transcript
                            : "Muse 官方语音助手已就绪，按住 BOOT 键开始对话。",
                        "Sans 16",
                        56,
                        152,
                        354,
                        PANGO_ALIGN_LEFT,
                        0.94,
                        0.96,
                        1.0);

        char cred_footer[128];
        snprintf(cred_footer,
                 sizeof(cred_footer),
                 "device_id: %s | token: %.16s…",
                 g_sim.muse_app.credentials.device_id,
                 g_sim.muse_app.credentials.access_token);
        draw_pango_text(cr, cred_footer, "Monospace 10", 52, 316, 362, PANGO_ALIGN_CENTER, 0.56, 0.52, 0.76);

        draw_rounded_rect(cr, 85, 360, 140, 48, 24);
        cairo_set_source_rgba(cr, 0.22, 0.14, 0.28, 1.0);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.6);
        cairo_set_source_rgba(cr, 0.58, 0.38, 0.78, 1.0);
        cairo_stroke(cr);
        draw_pango_text(cr, "解绑重授权", "Sans Bold 13", 90, 374, 130, PANGO_ALIGN_CENTER, 0.92, 0.88, 0.98);
    }

    draw_rounded_rect(cr, 241, 360, 140, 48, 24);
    cairo_set_source_rgba(cr, 0.10, 0.14, 0.20, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_set_source_rgba(cr, 0.25, 0.34, 0.46, 1.0);
    cairo_stroke(cr);
    draw_pango_text(cr, "切换系统", "Sans Bold 13", 246, 374, 130, PANGO_ALIGN_CENTER, 0.82, 0.88, 0.96);
}

static void render_nevoflux_screen(cairo_t *cr)
{
    /* 1. Top Status Bar: WiFi | Relay/Daemon | Sample Rate | Battery */
    char status_line[128];
    uint32_t sr_khz = bsp_audio_get_sample_rate() / 1000U;
    snprintf(status_line,
             sizeof(status_line),
             "%s · %s · %ukHz · 86%%",
             g_sim.pendant_cfg.has_wifi ? "WiFi" : "断网",
             g_sim.daemon_online ? "脑在线(peers:1)" : "脑离线(peers:0)",
             (unsigned)sr_khz);
    draw_pango_text(cr, status_line, "Sans Bold 11", 78, 54, 310, PANGO_ALIGN_CENTER, 0.58, 0.68, 0.78);

    /* 2. State Pill + Glowing Ring Indicator */
    const char *mode_title = "待机 · 按住 BOOT 说话";
    double mr = 0.18, mg = 0.78, mb = 0.85;
    if (g_sim.nevo_app.state == NF_APP_STATE_LISTENING) {
        mode_title = "聆听中… (NFA1 16kHz 上行)";
        mr = 0.12; mg = 0.92; mb = 0.55;
    } else if (g_sim.nevo_app.state == NF_APP_STATE_WAITING_REPLY) {
        mode_title = "思考中… (等待 speech_final / 推理)";
        mr = 0.98; mg = 0.72; mb = 0.18;
    } else if (g_sim.nevo_app.state == NF_APP_STATE_PLAYING) {
        mode_title = "TTS 播报中… (按住 BOOT 打断)";
        mr = 0.62; mg = 0.45; mb = 0.98;
    } else if (g_sim.nevo_app.state == NF_APP_STATE_MEETING) {
        mode_title = "● 会议录音中 (/meeting VAD 分段)";
        mr = 0.98; mg = 0.32; mb = 0.36;
    }

    /* Outer subtle glowing arc matching current mode */
    cairo_new_sub_path(cr);
    cairo_set_line_width(cr, 4.0);
    cairo_set_source_rgba(cr, mr, mg, mb, 0.55);
    cairo_arc(cr, 233.0, 233.0, 224.0, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    draw_rounded_rect(cr, 88, 82, 290, 38, 19);
    cairo_set_source_rgba(cr, mr * 0.22, mg * 0.22, mb * 0.22, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.8);
    cairo_set_source_rgba(cr, mr, mg, mb, 0.9);
    cairo_stroke(cr);
    draw_pango_text(cr, mode_title, "Sans Bold 13", 93, 91, 280, PANGO_ALIGN_CENTER, mr, mg, mb);

    /* 3. Center Transcript Card (Scrollable / Word-Wrapped CJK) */
    draw_rounded_rect(cr, 38, 132, 390, 210, 22);
    cairo_set_source_rgba(cr, 0.06, 0.09, 0.13, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.2);
    cairo_set_source_rgba(cr, 0.18, 0.24, 0.32, 1.0);
    cairo_stroke(cr);

    draw_pango_text(cr,
                    g_sim.nevo_app.screen_transcript,
                    "Sans 17",
                    56,
                    148,
                    354,
                    PANGO_ALIGN_LEFT,
                    0.94,
                    0.97,
                    1.0);

    /* Uplink / Downlink telemetry footer inside transcript box */
    char telemetry[128];
    snprintf(telemetry,
             sizeof(telemetry),
             "utt:%llx | chunk:%u | 500ms队列:%zu/25",
             (unsigned long long)g_sim.nevo_app.current_utterance_id,
             (unsigned)g_sim.nevo_app.current_chunk_seq,
             g_sim.nevo_app.uplink_q.count);
    draw_pango_text(cr, telemetry, "Monospace 10", 52, 316, 362, PANGO_ALIGN_CENTER, 0.44, 0.54, 0.66);

    /* 4. Bottom Touch Action Buttons inside Circular Safe Zone */
    bool in_meet = (g_sim.nevo_app.state == NF_APP_STATE_MEETING);
    draw_rounded_rect(cr, 85, 360, 140, 48, 24);
    cairo_set_source_rgba(cr, in_meet ? 0.42 : 0.10, in_meet ? 0.12 : 0.16, in_meet ? 0.15 : 0.22, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_set_source_rgba(cr, in_meet ? 0.98 : 0.25, in_meet ? 0.36 : 0.38, in_meet ? 0.38 : 0.52, 1.0);
    cairo_stroke(cr);
    draw_pango_text(cr,
                    in_meet ? "结束会议" : "会议模式",
                    "Sans Bold 13",
                    90,
                    374,
                    130,
                    PANGO_ALIGN_CENTER,
                    0.92,
                    0.95,
                    1.0);

    draw_rounded_rect(cr, 241, 360, 140, 48, 24);
    cairo_set_source_rgba(cr, 0.10, 0.14, 0.20, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_set_source_rgba(cr, 0.25, 0.34, 0.46, 1.0);
    cairo_stroke(cr);
    draw_pango_text(cr, "切换系统", "Sans Bold 13", 246, 374, 130, PANGO_ALIGN_CENTER, 0.82, 0.88, 0.96);
}

static void render_to_surface(cairo_surface_t *surface)
{
    cairo_t *cr = cairo_create(surface);

    /* Clear exterior corners outside the 466x466 round AMOLED bezel */
    cairo_set_source_rgb(cr, 0.03, 0.04, 0.06);
    cairo_paint(cr);

    /* Clip to 466x466 circle (radius = 230, center = 233, 233) */
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 233.0, 233.0, 230.0, 0.0, 2.0 * M_PI);
    cairo_clip(cr);

    /* Deep AMOLED pitch-black radial background */
    cairo_pattern_t *bg = cairo_pattern_create_radial(233, 180, 20, 233, 233, 240);
    cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.05, 0.08, 0.12);
    cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.01, 0.015, 0.025);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    if (g_sim.active_partition == SIM_PARTITION_LAUNCHER) {
        render_launcher_screen(cr);
    } else if (g_sim.active_partition == SIM_PARTITION_MUSE) {
        render_muse_screen(cr);
    } else {
        render_nevoflux_screen(cr);
    }

    cairo_restore(cr);

    /* Metallic watch bezel ring */
    cairo_new_sub_path(cr);
    cairo_set_line_width(cr, 3.0);
    cairo_set_source_rgba(cr, 0.28, 0.34, 0.42, 0.9);
    cairo_arc(cr, 233.0, 233.0, 230.5, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    cairo_destroy(cr);
}

bool sim_render_png(const char *png_path)
{
    if (!png_path) {
        return false;
    }
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BSP_LCD_H_RES, BSP_LCD_V_RES);
    render_to_surface(surface);
    cairo_status_t st = cairo_surface_write_to_png(surface, png_path);
    cairo_surface_destroy(surface);
    return (st == CAIRO_STATUS_SUCCESS);
}

int sim_get_state_json(char *out_buf, size_t out_cap)
{
    const char *part_name = (g_sim.active_partition == SIM_PARTITION_LAUNCHER) ? "factory (Launcher)"
                          : (g_sim.active_partition == SIM_PARTITION_MUSE)     ? "ota_0 (Muse Gadget SDK)"
                                                                               : "ota_1 (NevoFlux)";
    const char *app_st = "IDLE";
    if (g_sim.active_partition == SIM_PARTITION_MUSE) {
        app_st = (g_sim.muse_app.auth_state == MUSE_AUTH_AUTHORIZED)
               ? "MUSE_AUTHORIZED (NVS: muse_cfg)"
               : "MUSE_RFC8628_DEVICE_FLOW (user_code: ABC-123)";
    } else if (g_sim.nevo_app.state == NF_APP_STATE_LISTENING) app_st = "LISTENING (PTT)";
    else if (g_sim.nevo_app.state == NF_APP_STATE_WAITING_REPLY) app_st = "WAITING_REPLY";
    else if (g_sim.nevo_app.state == NF_APP_STATE_PLAYING) app_st = "PLAYING (TTS)";
    else if (g_sim.nevo_app.state == NF_APP_STATE_MEETING) app_st = "MEETING (/meeting)";

    return snprintf(out_buf,
                    out_cap,
                    "{"
                    "\"partition\":\"%s\","
                    "\"app_state\":\"%s\","
                    "\"muse_user_code\":\"%s\","
                    "\"sample_rate\":%u,"
                    "\"utterance_id\":\"%llu\","
                    "\"chunk_seq\":%u,"
                    "\"uplink_queue\":%zu,"
                    "\"tx_count\":%u,"
                    "\"last_tx\":%s"
                    "}",
                    part_name,
                    app_st,
                    g_sim.muse_app.user_code,
                    (unsigned)bsp_audio_get_sample_rate(),
                    (unsigned long long)g_sim.nevo_app.current_utterance_id,
                    (unsigned)g_sim.nevo_app.current_chunk_seq,
                    g_sim.nevo_app.uplink_q.count,
                    (unsigned)g_sim.tx_json_count,
                    g_sim.last_tx_json[0] ? g_sim.last_tx_json : "null");
}

#ifdef SIM_STANDALONE_MAIN
int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--dump-snapshots") == 0) {
        const char *out_dir = argv[2];
        char path[256];

        /* 1. Launcher screen */
        sim_init(0);
        snprintf(path, sizeof(path), "%s/sim_01_launcher.png", out_dir);
        sim_render_png(path);

        /* 2. Muse (ota_0) OAuth 2.0 Device Flow (RFC 8628 QR + ABC-123 short code) */
        sim_init(2);
        snprintf(path, sizeof(path), "%s/sim_05_muse_oauth_device_flow.png", out_dir);
        sim_render_png(path);

        /* 3. NevoFlux Listening (PTT) + Partial speech */
        sim_init(1);
        sim_set_boot_button(true);
        sim_step_ms(400);
        sim_inject_speech_partial("帮我查一下上周关于语音吊坠双分区架构的纪要…");
        snprintf(path, sizeof(path), "%s/sim_02_listening_ptt.png", out_dir);
        sim_render_png(path);

        /* 4. NevoFlux Playing 24kHz Kokoro TTS */
        sim_set_boot_button(false);
        sim_inject_speech_final("帮我查一下上周关于语音吊坠双分区架构的纪要。", true);
        sim_inject_tts_asset("已为您查到 GBrain 纪要：语音吊坠采用 8MB 三明治分区（Launcher + Muse + NevoFlux），语音全程在本地 Daemon 处理。", 24000U);
        snprintf(path, sizeof(path), "%s/sim_03_playing_24khz.png", out_dir);
        sim_render_png(path);

        /* 5. NevoFlux Hands-free Meeting Mode */
        sim_init(1);
        sim_trigger_double_click_boot();
        sim_step_ms(200);
        sim_inject_speech_final("【实时会议转写 #12】确定 10 月 22 日完成 NFA1 上行与 NFM1 下行端到端联调。", true);
        snprintf(path, sizeof(path), "%s/sim_04_meeting_mode.png", out_dir);
        sim_render_png(path);

        printf("[OK] Exported 5 circular 466x466 AMOLED snapshots to %s\n", out_dir);
        return 0;
    }

#ifdef USE_SDL2
    if (SDL_Init(SDL_INIT_VIDEO) == 0) {
        SDL_Window *win = SDL_CreateWindow("NevoFlux Pendant 1.43\" (466x466) Simulator",
                                           SDL_WINDOWPOS_CENTERED,
                                           SDL_WINDOWPOS_CENTERED,
                                           BSP_LCD_H_RES,
                                           BSP_LCD_V_RES,
                                           SDL_WINDOW_SHOWN);
        if (win) {
            SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!ren) {
                ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
            }
            SDL_Texture *tex = SDL_CreateTexture(ren,
                                                 SDL_PIXELFORMAT_ARGB8888,
                                                 SDL_TEXTUREACCESS_STREAMING,
                                                 BSP_LCD_H_RES,
                                                 BSP_LCD_V_RES);
            cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BSP_LCD_H_RES, BSP_LCD_V_RES);
            sim_init(0);
            printf("[SDL2] 466x466 Round AMOLED Window Started!\n");
            printf("       Mouse Click: Tap on-screen buttons\n");
            printf("       Keys: [1]=Launcher [2]=PTT [3]=TTS 24kHz [4]=Meeting [5]=Muse OAuth2 [Space]=Hold PTT [M]=Meeting [Q/Esc]=Quit\n");

            bool running = true;
            while (running) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_QUIT) running = false;
                    else if (ev.type == SDL_MOUSEBUTTONDOWN) sim_touch_xy(ev.button.x, ev.button.y);
                    else if (ev.type == SDL_KEYDOWN) {
                        SDL_Keycode k = ev.key.keysym.sym;
                        if (k == SDLK_ESCAPE || k == SDLK_q) running = false;
                        else if (k == SDLK_SPACE) sim_set_boot_button(true);
                        else if (k == SDLK_m) sim_trigger_double_click_boot();
                        else if (k == SDLK_1) sim_init(0);
                        else if (k == SDLK_2) {
                            sim_init(1);
                            sim_set_boot_button(true);
                            sim_step_ms(400);
                            sim_inject_speech_partial("帮我查一下上周关于语音吊坠双分区架构的纪要…");
                        } else if (k == SDLK_3) {
                            sim_init(1);
                            sim_inject_speech_final("帮我查一下上周关于语音吊坠双分区架构的纪要。", true);
                            sim_inject_tts_asset("已为您查到 GBrain 纪要：语音吊坠采用 8MB 三明治分区（Launcher + Muse + NevoFlux），语音全程在本地 Daemon 处理。", 24000U);
                        } else if (k == SDLK_4) {
                            sim_init(1);
                            sim_trigger_double_click_boot();
                            sim_step_ms(200);
                            sim_inject_speech_final("【实时会议转写 #12】确定 10 月 22 日完成 NFA1 上行与 NFM1 下行端到端联调。", true);
                        } else if (k == SDLK_5) {
                            sim_init(2);
                        }
                    } else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_SPACE) {
                        sim_set_boot_button(false);
                    }
                }
                sim_step_ms(20);
                render_to_surface(surf);
                SDL_UpdateTexture(tex, NULL, cairo_image_surface_get_data(surf), cairo_image_surface_get_stride(surf));
                SDL_RenderClear(ren);
                SDL_RenderCopy(ren, tex, NULL, NULL);
                SDL_RenderPresent(ren);
                SDL_Delay(20);
            }
            cairo_surface_destroy(surf);
            SDL_DestroyTexture(tex);
            SDL_DestroyRenderer(ren);
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 0;
        }
    }
#endif
    printf("No X11/Wayland DISPLAY detected for SDL2 window (%s); use `make sim-web` for Web UI or `--dump-snapshots <dir>`.\n", SDL_GetError());
    return 0;
}
#endif
