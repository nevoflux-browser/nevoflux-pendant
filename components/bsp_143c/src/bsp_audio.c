#include "bsp_143c.h"
#include <string.h>

#if defined(ESP_PLATFORM)
/*
 * ES8311 (speaker DAC) + ES7210 (mic ADC) on one I2S port, configured exactly like
 * the official Waveshare 04_Audio_Test (codec_board "S3_AMOLED_1_43C"):
 *   I2S0 master, TDM Philips, 32-bit slots, 4 slots, MCLK = 256 * fs
 *   ES8311: DAC mode, PA on GPIO46, external MCLK, pa_gain 6 dB
 *   ES7210: MIC1..MIC4 selected (TDM)
 * Both directions are opened as 2-channel 16-bit like the official demo; this file
 * converts to/from the mono 16-bit PCM used by NevoFlux (16 kHz up, WAV down).
 *
 * TX and RX share BCLK/WS, so esp_codec_dev refuses different sample rates on the
 * two directions: playing 24/48 kHz closes the mic, restore_capture_16k() reopens it.
 */
#include "bsp_internal.h"
#include "driver/gpio.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "bsp_audio";

#ifndef CONFIG_BSP_AUDIO_SPK_VOLUME
#define CONFIG_BSP_AUDIO_SPK_VOLUME 80
#endif
#ifndef CONFIG_BSP_AUDIO_MIC_GAIN_DB
#define CONFIG_BSP_AUDIO_MIC_GAIN_DB 30
#endif

static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static esp_codec_dev_handle_t s_play_dev;
static esp_codec_dev_handle_t s_rec_dev;
static bool s_play_open;
static bool s_rec_open;
static uint32_t s_play_rate;

/* Stereo scratch for mono <-> 2-channel conversion (grown on demand, PSRAM ok) */
static int16_t *s_scratch;
static size_t s_scratch_samples;

static bool scratch_reserve(size_t stereo_samples)
{
    if (stereo_samples <= s_scratch_samples) {
        return true;
    }
    int16_t *p = heap_caps_realloc(s_scratch, stereo_samples * sizeof(int16_t),
                                   MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!p) {
        p = realloc(s_scratch, stereo_samples * sizeof(int16_t));
    }
    if (!p) {
        return false;
    }
    s_scratch = p;
    s_scratch_samples = stereo_samples;
    return true;
}

static bool i2s_tdm_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* play silence on underrun instead of repeating stale DMA */
    if (i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan) != ESP_OK) {
        return false;
    }
    i2s_tdm_slot_mask_t slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3;
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_MIC_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, slot_mask),
        .gpio_cfg = {
            .mclk = BSP_PIN_I2S_MCLK,
            .bclk = BSP_PIN_I2S_BCLK,
            .ws   = BSP_PIN_I2S_WS,
            .dout = BSP_PIN_I2S_DOUT,
            .din  = BSP_PIN_I2S_DIN,
        },
    };
    tdm_cfg.slot_cfg.total_slot = 4;
    if (i2s_channel_init_tdm_mode(s_tx_chan, &tdm_cfg) != ESP_OK ||
        i2s_channel_init_tdm_mode(s_rx_chan, &tdm_cfg) != ESP_OK) {
        return false;
    }
    /* Clocks must run before the codecs are programmed */
    (void)i2s_channel_enable(s_tx_chan);
    (void)i2s_channel_enable(s_rx_chan);
    return true;
}

static bool codecs_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_bus();
    if (!bus) {
        return false;
    }
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BSP_I2S_NUM,
        .tx_handle = s_tx_chan,
        .rx_handle = s_rx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    /* esp_codec_dev expects 8-bit (shifted) I2C addresses: ES8311 0x30, ES7210 0x80 */
    audio_codec_i2c_cfg_t es8311_i2c = {
        .port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&es8311_i2c),
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_PIN_PA_CTRL,
        .use_mclk = true,
        .hw_gain = { .pa_gain = 6 },
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);

    audio_codec_i2c_cfg_t es7210_i2c = {
        .port = BSP_I2C_NUM, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus,
    };
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&es7210_i2c),
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);

    if (!data_if || !es8311_if || !es7210_if) {
        ESP_LOGE(TAG, "codec interface creation failed (ES8311 %p, ES7210 %p) - check I2C / codec power",
                 (void *)es8311_if, (void *)es7210_if);
        return false;
    }
    esp_codec_dev_cfg_t play_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_if, .data_if = data_if,
    };
    esp_codec_dev_cfg_t rec_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_if, .data_if = data_if,
    };
    s_play_dev = esp_codec_dev_new(&play_cfg);
    s_rec_dev = esp_codec_dev_new(&rec_cfg);
    return s_play_dev && s_rec_dev;
}

static esp_codec_dev_sample_info_t stereo16(uint32_t rate)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = rate,
    };
    return fs;
}

/* Lock held */
static bool rec_open_locked(void)
{
    if (s_rec_open) {
        return true;
    }
    esp_codec_dev_sample_info_t fs = stereo16(BSP_AUDIO_DEFAULT_MIC_RATE);
    if (esp_codec_dev_open(s_rec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES7210 open failed");
        return false;
    }
    (void)esp_codec_dev_set_in_gain(s_rec_dev, (float)CONFIG_BSP_AUDIO_MIC_GAIN_DB);
    s_rec_open = true;
    return true;
}

static void rec_close_locked(void)
{
    if (s_rec_open) {
        (void)esp_codec_dev_close(s_rec_dev);
        s_rec_open = false;
    }
}

static bool play_open_locked(uint32_t rate)
{
    if (s_play_open && s_play_rate == rate) {
        return true;
    }
    if (s_play_open) {
        (void)esp_codec_dev_close(s_play_dev); /* ES8311 close also drops the PA */
        s_play_open = false;
    }
    if (rate != BSP_AUDIO_DEFAULT_MIC_RATE) {
        rec_close_locked(); /* shared BCLK/WS cannot run two rates */
    }
    esp_codec_dev_sample_info_t fs = stereo16(rate);
    if (esp_codec_dev_open(s_play_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 open @%u Hz failed", (unsigned)rate);
        return false;
    }
    (void)esp_codec_dev_set_out_vol(s_play_dev, CONFIG_BSP_AUDIO_SPK_VOLUME);
    s_play_open = true;
    s_play_rate = rate;
    return true;
}

static void play_close_locked(void)
{
    if (s_play_open) {
        (void)esp_codec_dev_close(s_play_dev);
        s_play_open = false;
    }
}

static void audio_selftest(void);
#endif /* ESP_PLATFORM */

static uint32_t s_current_sample_rate = 0U;
static bool     s_pa_enabled = false;

void bsp_audio_set_pa_enable(bool enable)
{
    s_pa_enabled = enable;
#if defined(ESP_PLATFORM)
    gpio_set_level(BSP_PIN_PA_CTRL, enable ? 1 : 0);
#endif
}

uint32_t bsp_audio_get_sample_rate(void)
{
    return s_current_sample_rate;
}

bool bsp_audio_init(uint32_t default_sample_rate_hz)
{
    if (default_sample_rate_hz < 8000U || default_sample_rate_hz > 48000U) {
        return false;
    }

#if defined(ESP_PLATFORM)
    if (s_play_dev) {
        return true;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return false;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BSP_PIN_PA_CTRL) | (1ULL << BSP_PIN_CODEC_LDO_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    bsp_audio_set_pa_enable(false);
    gpio_set_level(BSP_PIN_CODEC_LDO_EN, 1); /* power ES8311 / ES7210 */
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!i2s_tdm_init() || !codecs_init()) {
        ESP_LOGE(TAG, "audio init failed");
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = rec_open_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "ES8311 + ES7210 ready (TDM, mic %u Hz, vol %d, gain %d dB)",
             (unsigned)BSP_AUDIO_DEFAULT_MIC_RATE, CONFIG_BSP_AUDIO_SPK_VOLUME, CONFIG_BSP_AUDIO_MIC_GAIN_DB);
    s_current_sample_rate = BSP_AUDIO_DEFAULT_MIC_RATE;
#if CONFIG_BSP_AUDIO_BOOT_SELFTEST
    audio_selftest();
#endif
    return ok;
#else
    s_current_sample_rate = default_sample_rate_hz;
    return true;
#endif
}

bool bsp_audio_set_play_sample_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz != 16000U && sample_rate_hz != 24000U && sample_rate_hz != 48000U) {
        return false;
    }
#if defined(ESP_PLATFORM)
    if (!s_play_dev) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = play_open_locked(sample_rate_hz);
    xSemaphoreGive(s_lock);
    if (!ok) {
        return false;
    }
#endif
    s_current_sample_rate = sample_rate_hz;
    return true;
}

bool bsp_audio_restore_capture_16k(void)
{
#if defined(ESP_PLATFORM)
    if (!s_play_dev) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    play_close_locked(); /* also mutes the PA: no hiss while listening */
    bool ok = rec_open_locked();
    xSemaphoreGive(s_lock);
    if (!ok) {
        return false;
    }
#else
    bsp_audio_set_pa_enable(false);
#endif
    s_current_sample_rate = BSP_AUDIO_DEFAULT_MIC_RATE;
    return true;
}

bool bsp_audio_read_mic(int16_t *pcm_out, size_t samples_to_read, size_t *out_samples_read, uint32_t timeout_ms)
{
    if (!pcm_out || !out_samples_read || samples_to_read == 0U) {
        return false;
    }
    *out_samples_read = 0U;
#if defined(ESP_PLATFORM)
    if (!s_rec_dev) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1U));
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_rec_open && scratch_reserve(samples_to_read * 2U) &&
              esp_codec_dev_read(s_rec_dev, s_scratch, (int)(samples_to_read * 2U * sizeof(int16_t))) ==
                  ESP_CODEC_DEV_OK;
    if (ok) {
        /* Channel 0 = first ES7210 mic */
        for (size_t i = 0; i < samples_to_read; ++i) {
            pcm_out[i] = s_scratch[2U * i];
        }
        *out_samples_read = samples_to_read;
    }
    xSemaphoreGive(s_lock);
    if (!ok) {
        /* Mic closed while playing 24/48 kHz: don't let the caller spin */
        vTaskDelay(pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1U));
    }
    return ok;
#else
    (void)timeout_ms;
    memset(pcm_out, 0, samples_to_read * sizeof(int16_t));
    *out_samples_read = samples_to_read;
    return true;
#endif
}

bool bsp_audio_write_spk(const int16_t *pcm_in, size_t samples_to_write, size_t *out_samples_written, uint32_t timeout_ms)
{
    if (!pcm_in || !out_samples_written || samples_to_write == 0U) {
        return false;
    }
    *out_samples_written = 0U;
#if defined(ESP_PLATFORM)
    (void)timeout_ms;
    if (!s_play_dev) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = play_open_locked(s_play_open ? s_play_rate : s_current_sample_rate) &&
              scratch_reserve(samples_to_write * 2U);
    if (ok) {
        for (size_t i = 0; i < samples_to_write; ++i) {
            s_scratch[2U * i] = pcm_in[i]; /* ES8311 is mono: feed the sample to both slots */
            s_scratch[2U * i + 1U] = pcm_in[i];
        }
        ok = esp_codec_dev_write(s_play_dev, s_scratch, (int)(samples_to_write * 2U * sizeof(int16_t))) ==
             ESP_CODEC_DEV_OK;
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        *out_samples_written = samples_to_write;
    }
    return ok;
#else
    (void)timeout_ms;
    *out_samples_written = samples_to_write;
    return true;
#endif
}

#if defined(ESP_PLATFORM)
/* Boot self-test: short 1 kHz beep, then 0.5 s of mic level per channel on the log. */
static void audio_selftest(void)
{
    enum { RATE = 16000, BEEP_MS = 150, FRAME = 320 };
    static const int16_t k_sine16[16] = {
        0, 3827, 7071, 9239, 10000, 9239, 7071, 3827, 0, -3827, -7071, -9239, -10000, -9239, -7071, -3827,
    };
    int16_t frame[FRAME];
    size_t done = 0;
    bool spk_ok = bsp_audio_set_play_sample_rate(RATE);
    for (int n = 0; spk_ok && n < RATE * BEEP_MS / 1000; n += FRAME) {
        for (int i = 0; i < FRAME; ++i) {
            frame[i] = k_sine16[(n + i) & 15]; /* 16 kHz / 16 = 1 kHz, ~ -10 dBFS */
        }
        spk_ok = bsp_audio_write_spk(frame, FRAME, &done, 100);
    }
    /* Flush the beep's tail with silence before closing the DAC */
    memset(frame, 0, sizeof(frame));
    for (int k = 0; spk_ok && k < 4; ++k) {
        spk_ok = bsp_audio_write_spk(frame, FRAME, &done, 100);
    }
    (void)bsp_audio_restore_capture_16k();
    ESP_LOGI(TAG, "self-test: speaker beep %s", spk_ok ? "played" : "FAILED");

    int64_t sum_sq[2] = {0, 0};
    int32_t peak[2] = {0, 0};
    size_t total = 0;
    bool mic_ok = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int f = 0; mic_ok && f < 25; ++f) { /* 25 x 20 ms */
        mic_ok = s_rec_open && scratch_reserve(FRAME * 2) &&
                 esp_codec_dev_read(s_rec_dev, s_scratch, FRAME * 2 * (int)sizeof(int16_t)) == ESP_CODEC_DEV_OK;
        if (f < 5) {
            continue; /* skip ADC settling */
        }
        for (int i = 0; mic_ok && i < FRAME; ++i) {
            for (int c = 0; c < 2; ++c) {
                int32_t v = s_scratch[2 * i + c];
                sum_sq[c] += (int64_t)v * v;
                if (v < 0) {
                    v = -v;
                }
                if (v > peak[c]) {
                    peak[c] = v;
                }
            }
        }
        total += FRAME;
    }
    xSemaphoreGive(s_lock);
    if (!mic_ok || total == 0) {
        ESP_LOGE(TAG, "self-test: mic read FAILED");
        return;
    }
    for (int c = 0; c < 2; ++c) {
        int32_t rms = 0;
        int64_t mean = sum_sq[c] / (int64_t)total;
        while ((int64_t)(rms + 1) * (rms + 1) <= mean) {
            rms++;
        }
        ESP_LOGI(TAG, "self-test: mic ch%d rms=%d peak=%d (0 = silent/dead, 32767 = clipping)", c,
                 (int)rms, (int)peak[c]);
    }
}
#endif
