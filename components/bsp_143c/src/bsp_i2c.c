#include "bsp_143c.h"
#include "bsp_internal.h"

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t bsp_i2c_get_bus(void)
{
    static StaticSemaphore_t s_lock_buf;
    static SemaphoreHandle_t s_lock;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_bus) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = BSP_I2C_NUM,
            .sda_io_num = BSP_PIN_I2C_SDA,
            .scl_io_num = BSP_PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&cfg, &s_bus) != ESP_OK) {
            ESP_LOGE("bsp_i2c", "I2C master bus init failed");
            s_bus = NULL;
        }
    }
    xSemaphoreGive(s_lock);
    return s_bus;
}
#endif
