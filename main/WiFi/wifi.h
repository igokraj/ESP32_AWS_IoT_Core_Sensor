#pragma once

#include "esp_err.h"

/* Start WiFi in station mode and wait up to timeout_ms for the first connection
   Returns ESP_OK when connected, ESP_ERR_TIMEOUT otherwise */
esp_err_t wifi_init_start(uint32_t timeout_ms);
void wifi_stop(void);
