#pragma once

#include "esp_err.h"

/* Start the MQTT client (call after WiFi is connected)
   The client manages reconnection on its own */
void mqtt_start(void);

/* Publish one sensor reading as JSON: {"t":23.45,"h":48.20}
   Returns ESP_ERR_INVALID_STATE when not connected to the broker */
esp_err_t mqtt_publish_reading(float temperature, float humidity);

/* Poll until the broker is connected, or give up after timeout_ms
   Needed because mqtt_start() connects asynchronously while deep sleep gives
   only one shot at publishing — without this wait the publish would be skipped */
esp_err_t mqtt_wait_for_connection(uint32_t timeout_ms);

/* Clean shutdown before deep sleep or a reboot. Closing the TLS/TCP connection
   politely keeps the broker from logging an abrupt drop, and keeps transport errors
   out of the next boot log */
void mqtt_stop(void);
