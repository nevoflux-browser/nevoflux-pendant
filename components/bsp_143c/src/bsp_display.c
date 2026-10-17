#include "bsp_143c.h"
#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"

static const void *s_font_mmap_ptr = NULL;
static esp_partition_mmap_handle_t s_font_mmap_handle;
#endif

static uint16_t *s_psram_fb = NULL;

bool bsp_display_init(void)
{
#if defined(ESP_PLATFORM)
    /* Allocate 466x466 RGB565 draw buffer in 8MB Octal PSRAM (never in internal SRAM) */
    size_t fb_bytes = (size_t)BSP_LCD_H_RES * (size_t)BSP_LCD_V_RES * sizeof(uint16_t);
    s_psram_fb = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_psram_fb) {
        return false;
    }
    memset(s_psram_fb, 0, fb_bytes);

    /* Zero-copy mmap of shared 20px CJK font in `storage` partition (0x6A0000) */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (part != NULL) {
        (void)esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                 &s_font_mmap_ptr, &s_font_mmap_handle);
    }
#else
    (void)s_psram_fb;
#endif
    return true;
}

void bsp_display_render_nevoflux(const bsp_ui_status_t *status)
{
    if (!status) {
        return;
    }
#if defined(ESP_PLATFORM)
    ESP_LOGI("bsp_disp", "[UI] WiFi:%d Relay:%d Peer:%d Bat:%u%% Mode:%s | %s",
             status->wifi_connected,
             status->relay_connected,
             status->daemon_online,
             status->battery_percent,
             status->mode_label ? status->mode_label : "",
             status->transcript ? status->transcript : "");
#endif
}

void bsp_display_render_launcher(int highlighted_system,
                                 const char *wifi_ssid,
                                 bool is_paired,
                                 const char *prompt)
{
#if defined(ESP_PLATFORM)
    ESP_LOGI("bsp_disp", "[Launcher] Sys:%d WiFi:%s Paired:%d | %s",
             highlighted_system,
             wifi_ssid ? wifi_ssid : "(none)",
             is_paired,
             prompt ? prompt : "");
#else
    (void)highlighted_system;
    (void)wifi_ssid;
    (void)is_paired;
    (void)prompt;
#endif
}

bsp_ui_touch_action_t bsp_display_poll_touch(void)
{
    return BSP_UI_Touch_NONE;
}
