#include "bsp_143c.h"
#include <string.h>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "esp_partition.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static adc_oneshot_unit_handle_t s_bat_adc = NULL;
static adc_cali_handle_t         s_bat_cali = NULL;

static void bsp_gpio_input_init(void)
{
    /* PWR (K2) drives the power-latch IC directly; only BOOT (GPIO0) is readable. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BSP_PIN_KEY_BOOT) | (1ULL << BSP_PIN_CHG_STAT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}
#endif

#define KEY_HOLD_THRESHOLD_MS       300ULL
#define KEY_DOUBLE_CLICK_WINDOW_MS  350ULL

bool bsp_reboot_to_launcher(void)
{
#if defined(ESP_PLATFORM)
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!otadata) {
        return false;
    }
    if (esp_partition_erase_range(otadata, 0, otadata->size) != ESP_OK) {
        return false;
    }
    esp_restart();
#endif
    return true;
}

void bsp_check_boot_escape_2s(void)
{
#if defined(ESP_PLATFORM)
    bsp_gpio_input_init();

    /* Sample GPIO0 (BOOT, active-low) every 50ms for 2000ms (40 ticks) */
    int held_ticks = 0;
    for (int i = 0; i < 40; ++i) {
        if (gpio_get_level(BSP_PIN_KEY_BOOT) == 0) {
            held_ticks++;
        } else {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (held_ticks >= 40) {
        /* Hard escape triggered: erase otadata and reboot into factory Launcher */
        (void)bsp_reboot_to_launcher();
    }
#endif
}

void bsp_key_init(bsp_key_fsm_t *fsm)
{
    if (!fsm) {
        return;
    }
    memset(fsm, 0, sizeof(*fsm));
#if defined(ESP_PLATFORM)
    bsp_gpio_input_init();
#endif
}

bsp_key_event_t bsp_key_Step(bsp_key_fsm_t *fsm, bool boot_pressed, bool pwr_pressed, uint64_t now_ms)
{
    if (!fsm) {
        return BSP_KEY_EVENT_NONE;
    }
    if (pwr_pressed) {
        return BSP_KEY_EVENT_PWR_CLICK;
    }

    /* 1. Rising edge of BOOT press */
    if (boot_pressed && !fsm->was_pressed) {
        fsm->was_pressed = true;
        fsm->hold_reported = false;
        fsm->press_start_ms = now_ms;
        return BSP_KEY_EVENT_NONE;
    }

    /* 2. Continuing to hold BOOT >= 300ms -> Push-to-Talk (PTT) start */
    if (boot_pressed && fsm->was_pressed) {
        if (!fsm->hold_reported && (now_ms - fsm->press_start_ms) >= KEY_HOLD_THRESHOLD_MS) {
            fsm->hold_reported = true;
            fsm->pending_clicks = 0U;
            return BSP_KEY_EVENT_BOOT_PRESS_START;
        }
        return BSP_KEY_EVENT_NONE;
    }

    /* 3. Falling edge (release) of BOOT */
    if (!boot_pressed && fsm->was_pressed) {
        fsm->was_pressed = false;
        if (fsm->hold_reported) {
            fsm->hold_reported = false;
            return BSP_KEY_EVENT_BOOT_RELEASE;
        }
        /* Short click released */
        if (fsm->pending_clicks == 1U &&
            (now_ms - fsm->last_click_release_ms) <= KEY_DOUBLE_CLICK_WINDOW_MS) {
            fsm->pending_clicks = 0U;
            return BSP_KEY_EVENT_BOOT_DOUBLE_CLICK;
        }
        fsm->pending_clicks = 1U;
        fsm->last_click_release_ms = now_ms;
        return BSP_KEY_EVENT_NONE;
    }

    /* 4. Idle: resolve single click once double-click window elapses */
    if (fsm->pending_clicks == 1U &&
        (now_ms - fsm->last_click_release_ms) > KEY_DOUBLE_CLICK_WINDOW_MS) {
        fsm->pending_clicks = 0U;
        return BSP_KEY_EVENT_BOOT_SINGLE_CLICK;
    }

    return BSP_KEY_EVENT_NONE;
}

bsp_key_event_t bsp_key_poll(bsp_key_fsm_t *fsm, uint64_t now_ms)
{
    bool boot_pressed = false;
    bool pwr_pressed = false;
#if defined(ESP_PLATFORM)
    boot_pressed = (gpio_get_level(BSP_PIN_KEY_BOOT) == 0);
#endif
    return bsp_key_Step(fsm, boot_pressed, pwr_pressed, now_ms);
}

uint8_t bsp_power_get_battery_percent(uint32_t *out_mv)
{
    uint32_t mv = 3950U;
#if defined(ESP_PLATFORM)
    if (!s_bat_adc) {
        adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
        adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_oneshot_new_unit(&unit_cfg, &s_bat_adc) != ESP_OK ||
            adc_oneshot_config_channel(s_bat_adc, (adc_channel_t)BSP_BAT_ADC_CHAN, &chan_cfg) != ESP_OK) {
            s_bat_adc = NULL;
        }
        (void)adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_bat_cali);
    }
    int raw = 0;
    int pin_mv = 0;
    if (s_bat_adc && s_bat_cali &&
        adc_oneshot_read(s_bat_adc, (adc_channel_t)BSP_BAT_ADC_CHAN, &raw) == ESP_OK &&
        adc_cali_raw_to_voltage(s_bat_cali, raw, &pin_mv) == ESP_OK) {
        mv = (uint32_t)((float)pin_mv * BSP_BAT_DIVIDER_RATIO);
    }
#endif
    if (out_mv) {
        *out_mv = mv;
    }
    if (mv <= 3300U) {
        return 0U;
    }
    if (mv >= 4200U) {
        return 100U;
    }
    return (uint8_t)(((mv - 3300U) * 100U) / 900U);
}
