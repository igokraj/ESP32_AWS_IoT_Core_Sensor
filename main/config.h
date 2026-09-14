#pragma once

#include "driver/gpio.h"

// ------- Board PIN map ------- 
#define I2C_MASTER_SCL_IO   GPIO_NUM_6
#define I2C_MASTER_SDA_IO    GPIO_NUM_7  
#define WIFI_LED_PIN GPIO_NUM_5 // Status LED for WiFi connection
#define MQTT_LED_PIN GPIO_NUM_4 // Status LED for MQTT

