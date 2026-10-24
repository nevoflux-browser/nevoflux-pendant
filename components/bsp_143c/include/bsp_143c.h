#ifndef BSP_143C_H
#define BSP_143C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bsp_143c_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. Audio Subsystem (ES8311 DAC + ES7210 Dual-Mic ADC over shared I2S_NUM_0)
 * ========================================================================= */

#define BSP_AUDIO_DEFAULT_MIC_RATE  16000U

bool bsp_audio_init(uint32_t default_sample_rate_hz);
uint32_t bsp_audio_get_sample_rate(void);

/**
 * Reconfigure shared I2S_NUM_0 clock and ES8311 DAC sample rate dynamically
 * (e.g. 24000Hz for Kokoro or 48000Hz for MOSS) with anti-pop PA muting.
 */
bool bsp_audio_set_play_sample_rate(uint32_t sample_rate_hz);

/**
 * Stop playback, flush DMA buffer, mute PA, and restore I2S_NUM_0 to 16kHz
 * for microphone capture. Called on playback completion or `voice_barge_in`.
 */
bool bsp_audio_restore_capture_16k(void);

bool bsp_audio_read_mic(int16_t *pcm_out, size_t samples_to_read, size_t *out_samples_read, uint32_t timeout_ms);
bool bsp_audio_write_spk(const int16_t *pcm_in, size_t samples_to_write, size_t *out_samples_written, uint32_t timeout_ms);
void bsp_audio_set_pa_enable(bool enable);

/* =========================================================================
 * 2. Display Subsystem (CO5300 466x466 QSPI + LVGL 9 UI + CST820 touch)
 * ========================================================================= */

typedef enum {
    BSP_UI_Touch_NONE         = 0,
    BSP_UI_TOUCH_SWITCH_SYS   = 1, /* Bottom button: Switch System (return to Launcher) */
    BSP_UI_TOUCH_TOGGLE_MEET  = 2, /* Bottom button: Toggle Meeting Mode */
    BSP_UI_TOUCH_BOOT_MUSE    = 3, /* Launcher card: Boot ota_0 (Muse) */
    BSP_UI_TOUCH_BOOT_NEVO    = 4, /* Launcher card: Boot ota_1 (NevoFlux) */
    BSP_UI_TOUCH_TOGGLE_BLE   = 5, /* Launcher bottom button: open/close BLE provisioning */
} bsp_ui_touch_action_t;

typedef struct {
    bool     wifi_connected;
    bool     relay_connected;
    bool     daemon_online;    /* true when portal-relay reports peers > 0 */
    uint8_t  battery_percent;  /* 0..100 */
    const char *mode_label;    /* e.g. "待机", "聆听中…", "思考中…", "播报中…", "会议录音中" */
    const char *transcript;    /* UTF-8 partial/final transcript or prompt */
} bsp_ui_status_t;

bool bsp_display_init(void);
void bsp_display_render_nevoflux(const bsp_ui_status_t *status);
void bsp_display_render_launcher(int highlighted_system, const char *wifi_ssid, bool is_paired, const char *prompt);
bsp_ui_touch_action_t bsp_display_poll_touch(void);

/** Generic screen: title, accent status line, wrapped body text + "switch system" button. */
void bsp_display_render_status(const char *title, const char *status, const char *body);

/** Network line shown under the title on every screen (e.g. "WiFi: MyAP · 192.168.1.23"). */
void bsp_display_set_net_info(const char *net_line);
/* Launcher BLE provisioning button: hidden when !available, highlighted while on. */
void bsp_display_set_ble_button(bool available, bool on);

/** AMOLED brightness 0..255 (CO5300 cmd 0x51). */
void bsp_display_set_brightness(uint8_t level);

/* =========================================================================
 * 3. Power, Battery ADC & BOOT Button Gesture / Escape Handler
 * ========================================================================= */

typedef enum {
    BSP_KEY_EVENT_NONE              = 0,
    BSP_KEY_EVENT_BOOT_PRESS_START  = 1, /* User pressed & held BOOT (start PTT) */
    BSP_KEY_EVENT_BOOT_RELEASE      = 2, /* User released BOOT (finish PTT) */
    BSP_KEY_EVENT_BOOT_SINGLE_CLICK = 3, /* Short click (confirm pairing offer) */
    BSP_KEY_EVENT_BOOT_DOUBLE_CLICK = 4, /* Double-click (toggle Meeting Mode) */
    BSP_KEY_EVENT_PWR_CLICK         = 5, /* PWR button click (cancel waiting_reply) */
} bsp_key_event_t;

typedef struct {
    bool     was_pressed;
    bool     hold_reported;
    uint64_t press_start_ms;
    uint64_t last_click_release_ms;
    uint8_t  pending_clicks;
} bsp_key_fsm_t;

/**
 * Early boot escape check: if BOOT (GPIO0) is held continuously for >= 2000ms
 * right at startup, erases `otadata` and restarts into `factory` (Launcher).
 */
void bsp_check_boot_escape_2s(void);

/**
 * Erase `otadata` partition and reboot into `factory` (Launcher).
 */
bool bsp_reboot_to_launcher(void);

void bsp_key_init(bsp_key_fsm_t *fsm);
bsp_key_event_t bsp_key_Step(bsp_key_fsm_t *fsm, bool boot_pressed, bool pwr_pressed, uint64_t now_ms);
bsp_key_event_t bsp_key_poll(bsp_key_fsm_t *fsm, uint64_t now_ms);

uint8_t bsp_power_get_battery_percent(uint32_t *out_mv);

#ifdef __cplusplus
}
#endif

#endif /* BSP_143C_H */
