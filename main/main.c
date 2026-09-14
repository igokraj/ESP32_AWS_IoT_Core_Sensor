#include <stdio.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

// --- USER .h FILES ---
#include "config.h"
#include "WiFi/wifi.h"
#include "sensor.h"

static const char *TAG = "app";

static void led_init(void) {
    gpio_reset_pin(WIFI_LED_PIN);
    gpio_set_direction(WIFI_LED_PIN, GPIO_MODE_OUTPUT);
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
    wifi_init_start();                  // Connect to WiFi (blocks until connected)

    if (sensor_init() != ESP_OK) {      // Initialize the HTU21D temp/hum sensor
        ESP_LOGE(TAG, "Sensor init failed");
        return;
    }

    while (1) {
        float t, h;
        if (sensor_read(&t, &h) == ESP_OK) {
            ESP_LOGI(TAG, "Temperature is: %.2f C, Humidity is: %.2f %%", t, h);
        } else {
            ESP_LOGW(TAG, "Sensor read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));    // Sleep 2 s, other tasks can run
    }
}
