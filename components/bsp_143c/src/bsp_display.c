#include "bsp_143c.h"
#include "bsp_internal.h"
#include <stdio.h>
#include <string.h>

static char s_net_line[96] = "WiFi: --";

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

LV_FONT_DECLARE(nf_font_20);
LV_FONT_DECLARE(nf_font_32);

static const char *TAG = "bsp_disp";

/* CO5300 init sequence from the official Waveshare 1.43C ESP-IDF demo
 * (06_LVGL_V9_Test/components/port_bsp/esp32_s3_touch_amoled_1.43c.cpp). */
static const sh8601_lcd_init_cmd_t s_co5300_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},  /* RGB565 */
    {0x35, (uint8_t[]){0x00}, 1, 0},  /* TE on */
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},  /* Brightness */
    {0x36, (uint8_t[]){0xC0}, 1, 0},  /* MADCTL: rotate 180 */
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100}, /* Sleep out */
    {0x29, (uint8_t[]){0x00}, 0, 0},   /* Display on */
};

#define UI_COLOR_BG      0x000000
#define UI_COLOR_FG      0xF2F2F2
#define UI_COLOR_DIM     0x8A8F98
#define UI_COLOR_ACCENT  0x4C8DFF
#define UI_COLOR_OK      0x3DDC84
#define UI_COLOR_WARN    0xFFB020
#define UI_COLOR_CARD    0x1A1C22
#define UI_COLOR_REC     0xFF4D4F

#define UI_TEXT_WIDTH    330 /* keeps wrapped text inside the round glass */

typedef enum {
    UI_SCREEN_NONE = 0,
    UI_SCREEN_LAUNCHER,
    UI_SCREEN_NEVOFLUX,
    UI_SCREEN_STATUS,
} ui_screen_t;

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static i2c_master_bus_handle_t   s_i2c_bus = NULL;
static i2c_master_dev_handle_t   s_touch_dev = NULL;
static lv_display_t             *s_disp = NULL;

static ui_screen_t s_screen = UI_SCREEN_NONE;
static volatile bsp_ui_touch_action_t s_pending_touch = BSP_UI_Touch_NONE;

/* Shared widgets */
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_net;
static lv_obj_t *s_lbl_body;
/* Launcher */
static lv_obj_t *s_btn_muse;
static lv_obj_t *s_btn_nevo;
static lv_obj_t *s_btn_ble;
static lv_obj_t *s_btn_mode; /* NevoFlux: chat <-> agent */
static int s_ble_btn_state = -1; /* -1 hidden, 0 off, 1 on (persists across screen rebuilds) */
static int s_ble_btn_applied = -2;
/* NevoFlux / status */
static lv_obj_t *s_lbl_mode;
static lv_obj_t *s_lbl_flags;
/* NevoFlux activity animation: level bars, or thinking dots */
#define UI_BARS          5
#define UI_DOTS          3
#define UI_BAR_W         20
#define UI_BAR_MIN_H     16
#define UI_BAR_MAX_H     120
#define UI_ANIM_Y        (-24)
static lv_obj_t *s_bars[UI_BARS];
static int32_t   s_bar_h[UI_BARS];
static lv_obj_t *s_dots[UI_DOTS];
static int       s_activity_applied = -1;


static void set_text_if_changed(lv_obj_t *lbl, const char *text)
{
    if (!lbl) {
        return;
    }
    const char *cur = lv_label_get_text(lbl);
    if (!text) {
        text = "";
    }
    if (!cur || strcmp(cur, text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

/* CO5300 requires even-aligned flush windows. */
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static bool touch_read_point(uint16_t *x, uint16_t *y)
{
    if (!s_touch_dev) {
        return false;
    }
    uint8_t reg = 0x02;
    uint8_t buf[5] = {0};
    /* 0x02: finger count, 0x03..0x06: XH XL YH YL (XH[7:6] = event, 1 = lift-up) */
    if (i2c_master_transmit_receive(s_touch_dev, &reg, 1, buf, sizeof(buf), 20) != ESP_OK) {
        return false;
    }
    if ((buf[0] & 0x0F) == 0 || (buf[1] >> 6) == 0x01) {
        return false;
    }
    uint16_t px = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    uint16_t py = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
    if (px >= BSP_LCD_H_RES || py >= BSP_LCD_V_RES) {
        return false;
    }
    /* The panel runs with MADCTL 0xC0 (rotated 180 deg), so the CST820 raw
     * coordinates are rotated too: map them as the official Waveshare demo
     * does (my_lvgl_indev_cb: LCD_WIDTH - x, LCD_HEIGHT - y). */
    *x = (uint16_t)(BSP_LCD_H_RES - 1 - px);
    *y = (uint16_t)(BSP_LCD_V_RES - 1 - py);
    return true;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x = 0;
    uint16_t y = 0;
    if (touch_read_point(&x, &y)) {
        data->point.x = (int32_t)x;
        data->point.y = (int32_t)y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void touch_hw_init(void)
{
    s_i2c_bus = bsp_i2c_get_bus(); /* shared with the ES8311 / ES7210 codecs */
    if (!s_i2c_bus) {
        ESP_LOGW(TAG, "I2C bus init failed; touch disabled");
        return;
    }

    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BSP_PIN_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(BSP_PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BSP_PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_I2C_ADDR_TOUCH_CST820,
        .scl_speed_hz = BSP_I2C_CLK_SPEED_HZ,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_touch_dev) != ESP_OK) {
        s_touch_dev = NULL;
        ESP_LOGW(TAG, "CST820 not attached; touch disabled");
    }
}

static bool panel_hw_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_PIN_LCD_PCLK,
        .data0_io_num = BSP_PIN_LCD_DATA0,
        .data1_io_num = BSP_PIN_LCD_DATA1,
        .data2_io_num = BSP_PIN_LCD_DATA2,
        .data3_io_num = BSP_PIN_LCD_DATA3,
        .max_transfer_sz = BSP_LCD_H_RES * 80 * (int)sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)BSP_LCD_QSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_PIN_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = BSP_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_QSPI_HOST, &io_cfg, &s_io));

    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_co5300_init_cmds,
        .init_cmds_size = sizeof(s_co5300_init_cmds) / sizeof(s_co5300_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, BSP_LCD_X_GAP, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    return true;
}

static void on_btn_clicked(lv_event_t *e)
{
    s_pending_touch = (bsp_ui_touch_action_t)(uintptr_t)lv_event_get_user_data(e);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t width)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    if (width > 0) {
        lv_obj_set_width(lbl, width);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    }
    lv_label_set_text(lbl, "");
    return lbl;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int32_t w, int32_t h,
                             bsp_ui_touch_action_t action)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_COLOR_CARD), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_btn_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)action);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &nf_font_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_FG), 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *make_blob(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void dot_bounce_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

/* Lock held. Show what `act` animates and hide the rest; the text is idle-only. */
static void apply_activity(bsp_ui_activity_t act)
{
    if (s_activity_applied == (int)act) {
        return;
    }
    s_activity_applied = (int)act;
    const bool bars = act == BSP_UI_ACT_LISTENING || act == BSP_UI_ACT_SPEAKING || act == BSP_UI_ACT_MEETING;
    const uint32_t color = act == BSP_UI_ACT_SPEAKING ? UI_COLOR_OK
                         : act == BSP_UI_ACT_MEETING  ? UI_COLOR_REC
                                                      : UI_COLOR_ACCENT;
    for (int i = 0; i < UI_BARS; ++i) {
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(color), 0);
        s_bar_h[i] = UI_BAR_MIN_H;
        lv_obj_set_height(s_bars[i], UI_BAR_MIN_H);
        if (bars) {
            lv_obj_remove_flag(s_bars[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < UI_DOTS; ++i) {
        lv_anim_delete(s_dots[i], dot_bounce_cb);
        lv_obj_set_style_translate_y(s_dots[i], 0, 0);
        if (act != BSP_UI_ACT_THINKING) {
            lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_dots[i]);
        lv_anim_set_exec_cb(&a, dot_bounce_cb);
        lv_anim_set_values(&a, 0, -30);
        lv_anim_set_duration(&a, 360);
        lv_anim_set_playback_duration(&a, 360);
        lv_anim_set_repeat_delay(&a, 300);
        lv_anim_set_delay(&a, (uint32_t)i * 180U);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    if (act == BSP_UI_ACT_IDLE) {
        lv_obj_align(s_lbl_mode, LV_ALIGN_CENTER, 0, -30);
        lv_obj_remove_flag(s_lbl_body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_align(s_lbl_mode, LV_ALIGN_CENTER, 0, UI_ANIM_Y + UI_BAR_MAX_H / 2 + 36);
        lv_obj_add_flag(s_lbl_body, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Lock held. Bars follow `level` (0..100), shaped high in the middle and
 * wobbling a little out of step so a steady voice still looks alive. */
static void update_bars(uint8_t level)
{
    static const int32_t shape[UI_BARS] = {55, 80, 100, 80, 55};
    const uint32_t t = lv_tick_get();
    const int32_t lvl = level > 100U ? 100 : (int32_t)level;
    for (int i = 0; i < UI_BARS; ++i) {
        int32_t wobble = lv_trigo_sin((int16_t)((t / 3U + (uint32_t)i * 75U) % 360U)); /* +-32767 */
        int32_t amp = lvl * shape[i] / 100;
        amp = amp * (85 + wobble * 15 / 32767) / 100;
        int32_t target = UI_BAR_MIN_H + (UI_BAR_MAX_H - UI_BAR_MIN_H) * amp / 100;
        /* Rise fast, fall slower */
        int32_t cur = s_bar_h[i];
        cur += (target > cur) ? (target - cur) / 2 : (target - cur) / 5;
        if (cur != s_bar_h[i]) {
            s_bar_h[i] = cur;
            lv_obj_set_height(s_bars[i], cur);
        }
    }
}

/* Must be called with the LVGL lock held. */
static void build_screen(ui_screen_t which)
{
    if (s_screen == which) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_btn_muse = s_btn_nevo = s_btn_ble = s_btn_mode = s_lbl_mode = s_lbl_flags = NULL;
    memset(s_bars, 0, sizeof(s_bars));
    memset(s_dots, 0, sizeof(s_dots));
    s_activity_applied = -1;
    s_ble_btn_applied = -2;

    s_lbl_title = make_label(scr, &nf_font_32, UI_COLOR_FG, 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 62);
    s_lbl_net = make_label(scr, &nf_font_20, UI_COLOR_DIM, 300);
    lv_obj_align(s_lbl_net, LV_ALIGN_TOP_MID, 0, 108);

    if (which == UI_SCREEN_LAUNCHER) {
        set_text_if_changed(s_lbl_title, "NevoFlux Pendant");
        s_btn_muse = make_button(scr, "Muse", 160, 72, BSP_UI_TOUCH_BOOT_MUSE);
        lv_obj_align(s_btn_muse, LV_ALIGN_CENTER, -88, 0);
        s_btn_nevo = make_button(scr, "NevoFlux", 160, 72, BSP_UI_TOUCH_BOOT_NEVO);
        lv_obj_align(s_btn_nevo, LV_ALIGN_CENTER, 88, 0);
        s_lbl_body = make_label(scr, &nf_font_20, UI_COLOR_FG, 300);
        lv_obj_align(s_lbl_body, LV_ALIGN_CENTER, 0, 86);
        s_btn_ble = make_button(scr, "开启蓝牙配网", 190, 50, BSP_UI_TOUCH_TOGGLE_BLE);
        lv_obj_align(s_btn_ble, LV_ALIGN_BOTTOM_MID, 0, -40);
    } else if (which == UI_SCREEN_NEVOFLUX) {
        set_text_if_changed(s_lbl_title, "NevoFlux");
        /* Single line: wrapping made the label jump between 1 and 2 lines (e.g. 99% -> 100%).
         * 400px fits the 466px circle's chord at y=138 (~424px). */
        s_lbl_flags = make_label(scr, &nf_font_20, UI_COLOR_DIM, 400);
        lv_label_set_long_mode(s_lbl_flags, LV_LABEL_LONG_DOT);
        lv_obj_align(s_lbl_flags, LV_ALIGN_TOP_MID, 0, 138);
        s_lbl_mode = make_label(scr, &nf_font_32, UI_COLOR_ACCENT, 0);
        lv_obj_align(s_lbl_mode, LV_ALIGN_CENTER, 0, -30);
        s_lbl_body = make_label(scr, &nf_font_20, UI_COLOR_FG, UI_TEXT_WIDTH);
        lv_obj_align(s_lbl_body, LV_ALIGN_CENTER, 0, 40);
        for (int i = 0; i < UI_BARS; ++i) {
            s_bars[i] = make_blob(scr, UI_BAR_W, UI_BAR_MIN_H);
            lv_obj_align(s_bars[i], LV_ALIGN_CENTER, (i - UI_BARS / 2) * 36, UI_ANIM_Y);
        }
        for (int i = 0; i < UI_DOTS; ++i) {
            s_dots[i] = make_blob(scr, 28, 28);
            lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(UI_COLOR_WARN), 0);
            lv_obj_align(s_dots[i], LV_ALIGN_CENTER, (i - UI_DOTS / 2) * 48, UI_ANIM_Y + 14);
        }
        lv_obj_t *b1 = make_button(scr, "切换系统", 136, 56, BSP_UI_TOUCH_SWITCH_SYS);
        lv_obj_align(b1, LV_ALIGN_BOTTOM_MID, -74, -64);
        lv_obj_t *b2 = make_button(scr, "会议", 136, 56, BSP_UI_TOUCH_TOGGLE_MEET);
        lv_obj_align(b2, LV_ALIGN_BOTTOM_MID, 74, -64);
        /* Below them, where the circle is still ~170 px wide */
        s_btn_mode = make_button(scr, "模式", 140, 44, BSP_UI_TOUCH_TOGGLE_MODE);
        lv_obj_align(s_btn_mode, LV_ALIGN_BOTTOM_MID, 0, -14);
    } else {
        s_lbl_mode = make_label(scr, &nf_font_20, UI_COLOR_ACCENT, 300);
        lv_obj_align(s_lbl_mode, LV_ALIGN_TOP_MID, 0, 150);
        s_lbl_body = make_label(scr, &nf_font_20, UI_COLOR_FG, UI_TEXT_WIDTH);
        lv_obj_align(s_lbl_body, LV_ALIGN_CENTER, 0, 30);
        lv_obj_t *b1 = make_button(scr, "切换系统", 150, 56, BSP_UI_TOUCH_SWITCH_SYS);
        lv_obj_align(b1, LV_ALIGN_BOTTOM_MID, 0, -56);
    }
    set_text_if_changed(s_lbl_net, s_net_line);
    s_screen = which;
}
#endif /* ESP_PLATFORM */

bool bsp_display_init(void)
{
#if defined(ESP_PLATFORM)
    if (s_disp) {
        return true;
    }
    if (!panel_hw_init()) {
        return false;
    }
    touch_hw_init();

    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = 0,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        /* 2 x 20 lines of internal DMA RAM (~37 KB); 40 lines starved Wi-Fi + TLS */
        .buffer_size = BSP_LCD_H_RES * 20,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        /* lvgl_port_add_disp() calls esp_lcd_panel_mirror(mirror_x, mirror_y), and the sh8601
         * driver maps mirror_x to MADCTL BIT6. mirror_x=true keeps the official 0xC0 MADCTL;
         * false would clear BIT6 (0x80) and show a mirrored image. */
        .rotation = { .swap_xy = false, .mirror_x = true, .mirror_y = false },
        .flags = { .buff_dma = true, .buff_spiram = false, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        return false;
    }

    if (lvgl_port_lock(0)) {
        lv_display_add_event_cb(s_disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
        if (s_touch_dev) {
            lv_indev_t *indev = lv_indev_create();
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(indev, touch_read_cb);
            lv_indev_set_display(indev, s_disp);
        }
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "CO5300 466x466 QSPI panel + LVGL %d.%d ready (touch=%s)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, s_touch_dev ? "CST820" : "off");
#endif
    return true;
}

void bsp_display_set_brightness(uint8_t level)
{
#if defined(ESP_PLATFORM)
    if (!s_io) {
        return;
    }
    /* QSPI write: 0x02 opcode, then 24-bit command address 0x00 0x51 0x00 */
    uint32_t cmd = (0x02U << 24) | (0x51U << 8);
    (void)esp_lcd_panel_io_tx_param(s_io, (int)cmd, &level, 1);
#else
    (void)level;
#endif
}

void bsp_display_set_net_info(const char *net_line)
{
    snprintf(s_net_line, sizeof(s_net_line), "%s", net_line ? net_line : "");
#if defined(ESP_PLATFORM)
    if (s_disp && lvgl_port_lock(100)) {
        set_text_if_changed(s_lbl_net, s_net_line);
        lvgl_port_unlock();
    }
#endif
}

void bsp_display_set_ble_button(bool available, bool on)
{
#if defined(ESP_PLATFORM)
    s_ble_btn_state = available ? (on ? 1 : 0) : -1; /* applied on the next render_launcher() */
#else
    (void)available;
    (void)on;
#endif
}

void bsp_display_render_nevoflux(const bsp_ui_status_t *status)
{
    if (!status) {
        return;
    }
#if defined(ESP_PLATFORM)
    if (!s_disp || !lvgl_port_lock(100)) {
        return;
    }
    build_screen(UI_SCREEN_NEVOFLUX);
    char flags[96];
    snprintf(flags, sizeof(flags), "%s %s · 主机 %s · 电量 %u%%",
             status->link_label ? status->link_label : "Relay",
             status->relay_connected ? "OK" : "--",
             status->daemon_online ? "在线" : "离线",
             (unsigned)status->battery_percent);
    set_text_if_changed(s_lbl_flags, flags);
    set_text_if_changed(s_lbl_mode, status->mode_label ? status->mode_label : "");
    set_text_if_changed(s_lbl_body, status->transcript ? status->transcript : "");
    if (s_btn_mode) {
        set_text_if_changed(lv_obj_get_child(s_btn_mode, 0),
                            status->session_mode_label ? status->session_mode_label : "模式");
    }
    apply_activity(status->activity);
    if (status->activity == BSP_UI_ACT_LISTENING || status->activity == BSP_UI_ACT_SPEAKING ||
        status->activity == BSP_UI_ACT_MEETING) {
        update_bars(status->level);
    }
    lvgl_port_unlock();
#endif
}

void bsp_display_render_launcher(int highlighted_system,
                                 const char *wifi_ssid,
                                 bool is_paired,
                                 const char *prompt)
{
#if defined(ESP_PLATFORM)
    (void)wifi_ssid; /* shown through bsp_display_set_net_info() with live link state */
    if (!s_disp || !lvgl_port_lock(100)) {
        return;
    }
    build_screen(UI_SCREEN_LAUNCHER);
    /* pendant_system_t: 0 = NevoFlux, 1 = Muse */
    lv_obj_set_style_border_color(s_btn_muse,
        lv_color_hex(highlighted_system == 1 ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
    lv_obj_set_style_border_color(s_btn_nevo,
        lv_color_hex(highlighted_system == 0 ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
    char body[192];
    snprintf(body, sizeof(body), "%s\n%s", is_paired ? "NevoFlux 已配对" : "NevoFlux 未配对",
             prompt ? prompt : "");
    set_text_if_changed(s_lbl_body, body);
    if (s_btn_ble && s_ble_btn_applied != s_ble_btn_state) {
        if (s_ble_btn_state < 0) {
            lv_obj_add_flag(s_btn_ble, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_btn_ble, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(s_btn_ble, 0),
                              s_ble_btn_state ? "关闭蓝牙配网" : "开启蓝牙配网");
            lv_obj_set_style_border_color(s_btn_ble,
                lv_color_hex(s_ble_btn_state ? UI_COLOR_ACCENT : UI_COLOR_CARD), 0);
        }
        s_ble_btn_applied = s_ble_btn_state;
    }
    lvgl_port_unlock();
#else
    (void)highlighted_system;
    (void)wifi_ssid;
    (void)is_paired;
    (void)prompt;
#endif
}

void bsp_display_render_status(const char *title, const char *status, const char *body)
{
#if defined(ESP_PLATFORM)
    if (!s_disp || !lvgl_port_lock(100)) {
        return;
    }
    build_screen(UI_SCREEN_STATUS);
    set_text_if_changed(s_lbl_title, title);
    set_text_if_changed(s_lbl_mode, status);
    set_text_if_changed(s_lbl_body, body);
    lvgl_port_unlock();
#else
    (void)title;
    (void)status;
    (void)body;
#endif
}

bsp_ui_touch_action_t bsp_display_poll_touch(void)
{
#if defined(ESP_PLATFORM)
    bsp_ui_touch_action_t a = s_pending_touch;
    s_pending_touch = BSP_UI_Touch_NONE;
    return a;
#else
    return BSP_UI_Touch_NONE;
#endif
}
