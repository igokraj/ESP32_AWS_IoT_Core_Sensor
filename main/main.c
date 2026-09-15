#include <stdio.h>
#include <string.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_system.h"     // esp_restart
#include "esp_app_desc.h"   // esp_app_get_description

// --- USER .h FILES ---
#include "config.h"
#include "WiFi/wifi.h"
#include "MQTT/mqtt.h"
#include "OTA/ota.h"
#include "sensor.h"

static const char *TAG = "app";

#define WAKEUP_TIME (60ULL * 1000000ULL)   // Deep sleep interval, in microseconds
#define WIFI_CONNECT_TIMEOUT_MS  15000      // Max time to wait for WiFi after wake-up
#define MQTT_CONNECT_TIMEOUT_MS  10000      // Max time to wait for the AWS broker
#define JOBS_TIMEOUT_MS          5000       // Max time to wait for an AWS IoT Jobs answer

static void led_init(void) {
    gpio_reset_pin(WIFI_LED_PIN);
    gpio_set_direction(WIFI_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(MQTT_LED_PIN);
    gpio_set_direction(MQTT_LED_PIN, GPIO_MODE_OUTPUT);
}

// Close connections cleanly and enter deep sleep. Never returns
static void go_to_sleep(void)
{
    mqtt_stop();    // Close the MQTT/TLS connection cleanly (safe if never started)
    wifi_stop();    // Release the WiFi association

    esp_sleep_enable_timer_wakeup(WAKEUP_TIME);
    ESP_LOGI(TAG, "Deep sleep for %llu s", WAKEUP_TIME / 1000000ULL);  // us -> s
    esp_deep_sleep_start();
}

/* First boot of an OTA-delivered image: keep it only if it can reach AWS
   The decision must happen now — any reset (deep sleep included) of an unconfirmed
   image makes the bootloader roll back. Does nothing for a USB-flashed image */
static void confirm_new_firmware(bool aws_reachable)
{
    if (ota_confirm_or_rollback(aws_reachable) == OTA_DIAGNOSTIC_FAILED) {
        mqtt_stop();
        wifi_stop();
        ota_rollback_and_reboot();      // never returns
    }
}

/* Ask AWS IoT Jobs for a pending firmware job and act on it
   The job document holds the target version, so one job covers the whole cycle:
   download -> reboot -> new version reports SUCCEEDED (or old one reports FAILED after a rollback) */
static void handle_ota_job(void)
{
    const mqtt_job_t *job = mqtt_get_next_job(JOBS_TIMEOUT_MS);
    if (job == NULL) {
        return;
    }

    const char *running = esp_app_get_description()->version;
    if (strcmp(job->version, running) == 0) {
        // Already running the job's version: the update went through
        mqtt_update_job_status(job->id, "SUCCEEDED", JOBS_TIMEOUT_MS);
        return;
    }
    if (ota_is_version_blacklisted(job->version)) {
        // This version was installed before, failed diagnostics and got rolled back
        mqtt_update_job_status(job->id, "FAILED", JOBS_TIMEOUT_MS);
        return;
    }

    switch (ota_check_and_update(job->url)) {
    case OTA_RESULT_UPDATE_READY:
        // Job stays IN_PROGRESS; the new firmware reports SUCCEEDED after it boots
        mqtt_stop();
        wifi_stop();
        esp_restart();
        break;
    case OTA_RESULT_NO_UPDATE:
        // The file in S3 does not contain the version named in the job
        ESP_LOGE(TAG, "firmware.bin version does not match job version %s", job->version);
        mqtt_update_job_status(job->id, "FAILED", JOBS_TIMEOUT_MS);
        break;
    case OTA_RESULT_ERROR:
        // Network problem — job stays IN_PROGRESS, retried on the next wake with a fresh URL
        break;
    }
}

void app_main(void)
{
    led_init();

    /* Initialize NVS: required by the WiFi driver (radio calibration data),
    and used by ota.c to store the black-listed firmware version */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Firmware version %s", esp_app_get_description()->version);

    ESP_LOGI(TAG, "starting WiFi... (STA mode)");
    if (wifi_init_start(WIFI_CONNECT_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "No WiFi, trying again after sleep");
        confirm_new_firmware(false);    // a fresh OTA image that cannot connect is rolled back
        go_to_sleep();                  // Don't keep the radio on with the router off
    }
    mqtt_start();                       // Start the MQTT client (connects asynchronously)

    float t, h;
    bool sensor_ok = false;
    if (sensor_init() == ESP_OK) {      // Initialize the HTU21D temp/hum sensor
        sensor_ok = (sensor_read(&t, &h) == ESP_OK);
    }

    bool mqtt_ok = (mqtt_wait_for_connection(MQTT_CONNECT_TIMEOUT_MS) == ESP_OK);
    confirm_new_firmware(mqtt_ok);      // reaching AWS is what future updates depend on

    if (sensor_ok) {
        ESP_LOGI(TAG, "Temperature is: %.2f C, Humidity is: %.2f %%", t, h);
        if (mqtt_ok) {
            mqtt_publish_reading(t, h);
        } else {
            ESP_LOGW(TAG, "MQTT not connected, reading dropped");
        }
    } else {
        ESP_LOGW(TAG, "Sensor read failed");
    }

    if (mqtt_ok) {
        handle_ota_job();
    }

    go_to_sleep();
}
