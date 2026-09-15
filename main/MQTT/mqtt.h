#pragma once

#include "esp_err.h"

/* One OTA job received from AWS IoT Jobs
   Job document created in AWS: {"version":"1.0.1","url":"${aws:iot:s3-presigned-url:...}"} */
typedef struct {
    char id[65];        // AWS job ID, max 64 chars
    char version[32];   // same size as esp_app_desc_t.version
    char url[3072];     // presigned S3 URL
} mqtt_job_t;

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

/* Ask AWS IoT Jobs for the next pending job (IN_PROGRESS first, then QUEUED)
   Returns NULL when there is no job or AWS did not answer within timeout_ms
   The returned job stays valid until the next call */
const mqtt_job_t *mqtt_get_next_job(uint32_t timeout_ms);

/* Report a job result to AWS: "IN_PROGRESS", "SUCCEEDED", "FAILED" or "REJECTED"
   Waits for the broker's confirmation, so it is safe to reboot or sleep right after */
esp_err_t mqtt_update_job_status(const char *job_id, const char *status, uint32_t timeout_ms);

/* Clean shutdown before deep sleep or a reboot. Closing the TLS/TCP connection
   politely keeps the broker from logging an abrupt drop, and keeps transport errors
   out of the next boot log */
void mqtt_stop(void);
