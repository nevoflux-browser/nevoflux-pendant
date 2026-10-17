#ifndef BSP_143C_PINS_H
#define BSP_143C_PINS_H

/* =========================================================================
 * Waveshare ESP32-S3-Touch-AMOLED-1.43C Hardware Pin Definitions
 * SoC: ESP32-S3-PICO-1-N8R8 (8MB Flash / 8MB Octal PSRAM)
 * ========================================================================= */

/* 1.43" Round AMOLED Display (SH8601, 466x466, QSPI) */
#define BSP_LCD_H_RES               466
#define BSP_LCD_V_RES               466
#define BSP_LCD_QSPI_HOST           1   /* SPI2_HOST */
#define BSP_PIN_LCD_CS              9
#define BSP_PIN_LCD_PCLK            10
#define BSP_PIN_LCD_DATA0           11
#define BSP_PIN_LCD_DATA1           12
#define BSP_PIN_LCD_DATA2           13
#define BSP_PIN_LCD_DATA3           14
#define BSP_PIN_LCD_RST             21
#define BSP_PIN_LCD_EN              42

/* Shared I2C Bus (Touch FT3168/CST816 + Audio ES8311/ES7210) */
#define BSP_I2C_NUM                 0
#define BSP_I2C_CLK_SPEED_HZ        400000
#define BSP_PIN_I2C_SDA             47
#define BSP_PIN_I2C_SCL             48
#define BSP_PIN_TOUCH_INT           40
#define BSP_PIN_TOUCH_RST           39

#define BSP_I2C_ADDR_ES8311         0x18
#define BSP_I2C_ADDR_ES7210         0x40
#define BSP_I2C_ADDR_TOUCH_FT3168   0x38
#define BSP_I2C_ADDR_TOUCH_CST816   0x15

/* Audio I2S Bus (I2S_NUM_0 shared clock between ES8311 DAC & ES7210 Dual-Mic ADC) */
#define BSP_I2S_NUM                 0
#define BSP_PIN_I2S_MCLK            38
#define BSP_PIN_I2S_BCLK            17
#define BSP_PIN_I2S_WS              18
#define BSP_PIN_I2S_DOUT            15  /* To ES8311 DAC */
#define BSP_PIN_I2S_DIN             16  /* From ES7210 ADC */
#define BSP_PIN_PA_CTRL             46  /* Power Amplifier Enable (Active High) */

/* Buttons & Battery ADC (No PMU; direct resistor divider to ADC1) */
#define BSP_PIN_KEY_BOOT            0   /* GPIO0: Active Low (Hold=PTT, DoubleClick=Meeting, Boot 2s=Escape) */
#define BSP_PIN_KEY_PWR             1   /* GPIO1: Cancel / Power Return */
#define BSP_BAT_ADC_UNIT            0   /* ADC_UNIT_1 */
#define BSP_BAT_ADC_CHAN            3   /* GPIO4 / ADC1_CH3 */
#define BSP_BAT_DIVIDER_RATIO       2.0f

#endif /* BSP_143C_PINS_H */
