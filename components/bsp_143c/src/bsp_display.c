#include "bsp_143c.h"
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
/* NevoFlux / status */
static lv_obj_t *s_lbl_mode;
static lv_obj_t *s_lbl_flags;


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
    /* With MADCTL 0xC0 the CST820 raw coordinates already match the panel
     * (same as the official Waveshare demo). */
    *x = px;
    *y = py;
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
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_PIN_I2C_SDA,
        .scl_io_num = BSP_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
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
    s_btn_muse = s_btn_nevo = s_lbl_mode = s_lbl_flags = NULL;

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
        lv_obj_align(s_lbl_body, LV_ALIGN_CENTER, 0, 100);
    } else if (which == UI_SCREEN_NEVOFLUX) {
        set_text_if_changed(s_lbl_title, "NevoFlux");
        s_lbl_flags = make_label(scr, &nf_font_20, UI_COLOR_DIM, 300);
        lv_obj_align(s_lbl_flags, LV_ALIGN_TOP_MID, 0, 138);
        s_lbl_mode = make_label(scr, &nf_font_32, UI_COLOR_ACCENT, 0);
        lv_obj_align(s_lbl_mode, LV_ALIGN_CENTER, 0, -30);
        s_lbl_body = make_label(scr, &nf_font_20, UI_COLOR_FG, UI_TEXT_WIDTH);
        lv_obj_align(s_lbl_body, LV_ALIGN_CENTER, 0, 40);
        lv_obj_t *b1 = make_button(scr, "切换系统", 136, 56, BSP_UI_TOUCH_SWITCH_SYS);
        lv_obj_align(b1, LV_ALIGN_BOTTOM_MID, -74, -64);
        lv_obj_t *b2 = make_button(scr, "会议", 136, 56, BSP_UI_TOUCH_TOGGLE_MEET);
        lv_obj_align(b2, LV_ALIGN_BOTTOM_MID, 74, -64);
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
        .buffer_size = BSP_LCD_H_RES * 40,
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
    snprintf(flags, sizeof(flags), "Relay %s · 主机 %s · 电量 %u%%",
             status->relay_connected ? "OK" : "--",
             status->daemon_online ? "在线" : "离线",
             (unsigned)status->battery_percent);
    set_text_if_changed(s_lbl_flags, flags);
    set_text_if_changed(s_lbl_mode, status->mode_label ? status->mode_label : "");
    set_text_if_changed(s_lbl_body, status->transcript ? status->transcript : "");
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
