#ifndef BSP_143C_PINS_H
#define BSP_143C_PINS_H

/* =========================================================================
 * Waveshare ESP32-S3-Touch-AMOLED-1.43C Hardware Pin Definitions
 * SoC: ESP32-S3-PICO-1-N8R8 (8MB Flash / 8MB Octal PSRAM)
 *
 * Source of truth: official Waveshare demo `bsp_config.h` and schematic
 * https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.43C
 *   02_Example/espidf_v5.5.3/06_LVGL_V9_Test/components/port_bsp/bsp_config.h
 * ========================================================================= */

/* 1.43" Round AMOLED Display (CO5300, 466x466, QSPI on SPI2_HOST).
 * CO5300 is command-compatible with SH8601 and is driven via esp_lcd_sh8601. */
#define BSP_LCD_H_RES               466
#define BSP_LCD_V_RES               466
#define BSP_LCD_X_GAP               8   /* CO5300 column offset (esp_lcd_panel_set_gap) */
#define BSP_LCD_QSPI_HOST           1   /* SPI2_HOST */
#define BSP_LCD_PCLK_HZ             (40 * 1000 * 1000)
#define BSP_PIN_LCD_CS              15
#define BSP_PIN_LCD_PCLK            14
#define BSP_PIN_LCD_DATA0           9
#define BSP_PIN_LCD_DATA1           10
#define BSP_PIN_LCD_DATA2           11
#define BSP_PIN_LCD_DATA3           12
#define BSP_PIN_LCD_RST             13
#define BSP_PIN_LCD_TE              8

/* Shared I2C Bus (Touch CST820 + Audio ES8311/ES7210) */
#define BSP_I2C_NUM                 0
#define BSP_I2C_CLK_SPEED_HZ        400000
#define BSP_PIN_I2C_SDA             47
#define BSP_PIN_I2C_SCL             48
#define BSP_PIN_TOUCH_INT           17
#define BSP_PIN_TOUCH_RST           16

#define BSP_I2C_ADDR_ES8311         0x18
#define BSP_I2C_ADDR_ES7210         0x40
#define BSP_I2C_ADDR_TOUCH_CST820   0x15

/* Audio I2S Bus (I2S_NUM_0 shared clock between ES8311 DAC & ES7210 Dual-Mic ADC) */
#define BSP_I2S_NUM                 0
#define BSP_PIN_I2S_MCLK            38
#define BSP_PIN_I2S_BCLK            39
#define BSP_PIN_I2S_WS              40
#define BSP_PIN_I2S_DOUT            41  /* ESP32 -> ES8311 DSDIN */
#define BSP_PIN_I2S_DIN             42  /* ES7210 DSOUT -> ESP32 */
#define BSP_PIN_PA_CTRL             46  /* NS4150 Power Amplifier Enable (Active High) */
#define BSP_PIN_CODEC_LDO_EN        18  /* Codec LDO Enable (Active High) */

/* Buttons, Battery ADC & Charger (No PMU; direct resistor divider to ADC1) */
#define BSP_PIN_KEY_BOOT            0   /* GPIO0: Active Low (Hold=PTT, DoubleClick=Meeting, Boot 2s=Escape) */
#define BSP_PIN_KEY_PWR             (-1) /* PWR key is wired to the power latch, not to a GPIO */
#define BSP_PIN_LED                 5   /* User LED */
#define BSP_PIN_CHG_STAT            7   /* ETA6098 charge status: Low = charging */
#define BSP_BAT_ADC_UNIT            0   /* ADC_UNIT_1 */
#define BSP_BAT_ADC_CHAN            3   /* GPIO4 / ADC1_CH3 */
#define BSP_BAT_DIVIDER_RATIO       2.0f

#endif /* BSP_143C_PINS_H */
