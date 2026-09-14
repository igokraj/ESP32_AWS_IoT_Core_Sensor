#include <stdio.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_sleep.h"

// --- USER .h FILES ---
#include "config.h"
#include "WiFi/wifi.h"
#include "MQTT/mqtt.h"
#include "sensor.h"

static const char *TAG = "app";

#define WAKEUP_TIME (20ULL * 1000000ULL)   // Deep sleep interval, in microseconds
#define WIFI_CONNECT_TIMEOUT_MS  15000      // Max time to wait for WiFi after wake-up

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

    ESP_LOGI(TAG, "starting WiFi... (STA mode)");
    if (wifi_init_start(WIFI_CONNECT_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "No WiFi, trying again after sleep");
        go_to_sleep();              // Don't keep the radio on with the router off
    }
    mqtt_start();                   // Start the MQTT client (connects asynchronously)

    float t, h;
    bool sensor_ok = false;
    if (sensor_init() == ESP_OK) {  // Initialize the HTU21D temp/hum sensor
        sensor_ok = (sensor_read(&t, &h) == ESP_OK);
    }

    if (sensor_ok) {
        ESP_LOGI(TAG, "Temperature is: %.2f C, Humidity is: %.2f %%", t, h);
        if (mqtt_wait_for_connection(10000) == ESP_OK) {
            mqtt_publish_reading(t, h);
        } else {
            ESP_LOGW(TAG, "MQTT not connected, reading dropped");
        }
    } else {
        ESP_LOGW(TAG, "Sensor read failed");
    }

    go_to_sleep();
}
