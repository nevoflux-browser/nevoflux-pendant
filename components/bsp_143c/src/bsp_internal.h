#ifndef BSP_143C_INTERNAL_H
#define BSP_143C_INTERNAL_H

#if defined(ESP_PLATFORM)
#include "driver/i2c_master.h"

/* Shared I2C master bus (SDA 47 / SCL 48): CST820 touch, ES8311, ES7210.
 * Created on first use; NULL if the bus could not be created. */
i2c_master_bus_handle_t bsp_i2c_get_bus(void);
#endif

#endif /* BSP_143C_INTERNAL_H */
