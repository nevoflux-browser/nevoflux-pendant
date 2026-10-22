#include "bsp_143c.h"
#include <string.h>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
#endif

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
    gpio_config_t pa_io = {
        .pin_bit_mask = (1ULL << BSP_PIN_PA_CTRL) | (1ULL << BSP_PIN_CODEC_LDO_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_io);
    gpio_set_level(BSP_PIN_CODEC_LDO_EN, 1); /* Power ES8311/ES7210 */
    bsp_audio_set_pa_enable(false);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 320; /* 20ms @ 16kHz */
    if (i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan) != ESP_OK) {
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(default_sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_PIN_I2S_MCLK,
            .bclk = BSP_PIN_I2S_BCLK,
            .ws   = BSP_PIN_I2S_WS,
            .dout = BSP_PIN_I2S_DOUT,
            .din  = BSP_PIN_I2S_DIN,
        },
    };
    if (i2s_channel_init_std_mode(s_tx_chan, &std_cfg) != ESP_OK ||
        i2s_channel_init_std_mode(s_rx_chan, &std_cfg) != ESP_OK) {
        return false;
    }
    (void)i2s_channel_enable(s_tx_chan);
    (void)i2s_channel_enable(s_rx_chan);
#endif

    s_current_sample_rate = default_sample_rate_hz;
    return true;
}

bool bsp_audio_set_play_sample_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz != 16000U && sample_rate_hz != 24000U && sample_rate_hz != 48000U) {
        return false;
    }

    if (s_current_sample_rate == sample_rate_hz) {
        bsp_audio_set_pa_enable(true);
        return true;
    }

    /* Anti-pop sequence:
     * 1. Mute PA before touching shared I2S_NUM_0 BCLK/WS clocks
     * 2. Disable TX/RX channels and reconfigure standard clock
     * 3. Re-enable TX channel, wait 10ms settle, then unmute PA
     */
    bsp_audio_set_pa_enable(false);

#if defined(ESP_PLATFORM)
    if (s_tx_chan) {
        (void)i2s_channel_disable(s_tx_chan);
    }
    if (s_rx_chan) {
        (void)i2s_channel_disable(s_rx_chan);
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    if (s_tx_chan) {
        (void)i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
        (void)i2s_channel_enable(s_tx_chan);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    s_current_sample_rate = sample_rate_hz;
    bsp_audio_set_pa_enable(true);
    return true;
}

bool bsp_audio_restore_capture_16k(void)
{
    bsp_audio_set_pa_enable(false);

#if defined(ESP_PLATFORM)
    if (s_tx_chan) {
        (void)i2s_channel_disable(s_tx_chan);
    }
    if (s_rx_chan) {
        (void)i2s_channel_disable(s_rx_chan);
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BSP_AUDIO_DEFAULT_MIC_RATE);
    if (s_tx_chan) {
        (void)i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
        (void)i2s_channel_enable(s_tx_chan);
    }
    if (s_rx_chan) {
        (void)i2s_channel_reconfig_std_clock(s_rx_chan, &clk_cfg);
        (void)i2s_channel_enable(s_rx_chan);
    }
#endif

    s_current_sample_rate = BSP_AUDIO_DEFAULT_MIC_RATE;
    return true;
}

bool bsp_audio_read_mic(int16_t *pcm_out, size_t samples_to_read, size_t *out_samples_read, uint32_t timeout_ms)
{
    if (!pcm_out || !out_samples_read || samples_to_read == 0U) {
        return false;
    }
#if defined(ESP_PLATFORM)
    size_t bytes_read = 0U;
    esp_err_t rc = i2s_channel_read(s_rx_chan,
                                    pcm_out,
                                    samples_to_read * sizeof(int16_t),
                                    &bytes_read,
                                    pdMS_TO_TICKS(timeout_ms));
    *out_samples_read = bytes_read / sizeof(int16_t);
    return (rc == ESP_OK);
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
#if defined(ESP_PLATFORM)
    size_t bytes_written = 0U;
    esp_err_t rc = i2s_channel_write(s_tx_chan,
                                     pcm_in,
                                     samples_to_write * sizeof(int16_t),
                                     &bytes_written,
                                     pdMS_TO_TICKS(timeout_ms));
    *out_samples_written = bytes_written / sizeof(int16_t);
    return (rc == ESP_OK);
#else
    (void)timeout_ms;
    *out_samples_written = samples_to_write;
    return true;
#endif
}
