#pragma once    // add this file only once

#include "esp_err.h"

// Initialize the I2C bus and the device (HTU21D sensor)
esp_err_t sensor_init(void);

/* Trigger a measurement and return temperature and relative humidity
   Either pointer may be NULL if that value is not needed */
esp_err_t sensor_read(float *temperature_c, float *humidity_pct);
