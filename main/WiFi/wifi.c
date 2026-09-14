#include "wifi.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "inttypes.h"

#include "config.h"     // LED_PINs
#include "secrets.h"    // WIFI_SSID, WIFI_PASS

/* Set by wifi_stop() before a deliberate shutdown. volatile because it is written
   from app_main but read by the event handler, which runs in the event-loop task */
static volatile bool s_stopping = false;

static const char *TAG = "wifi";

/* Set by event_handler() once we are connected to the AP with an IP
   There is no "fail" bit: the reconnect timer retries forever, so there is no
   give-up state */
#define WIFI_CONNECTED_BIT BIT0

static int s_retry_num = 0;
static esp_timer_handle_t s_reconnect_timer;
static EventGroupHandle_t s_wifi_event_group;

// One-shot timer callback: fires after the backoff delay and retries the connection
static void reconnect_cb(void *arg) {
    esp_wifi_connect();
}

/* Handles WiFi and IP events: drives the status LED, and reconnects with backoff
   Runs in the event-loop task, so it must never block (no vTaskDelay) */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
   } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_stopping) {
            return;     // We disconnected on purpose: skip the pointless reconnect + log
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        gpio_set_level(WIFI_LED_PIN, 0);                                    // Status LED: WiFi not connected
        uint32_t delay_ms = 1000 << (s_retry_num < 5 ? s_retry_num : 5);    // 1s ... 32s
        if (s_retry_num < 5) s_retry_num++;
        ESP_LOGW(TAG, "disconnected, retry in %" PRIu32 " ms", delay_ms);
        // Non-blocking backoff: arm a one-shot timer instead of blocking the event loop
        esp_err_t err = esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reconnect timer not armed: %s", esp_err_to_name(err));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        gpio_set_level(WIFI_LED_PIN, 1);                                    // Status LED: WiFi connected
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Set up WiFi in station mode and wait up to timeout_ms for the first connection
esp_err_t wifi_init_start(uint32_t timeout_ms)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
#ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
            .disable_wpa3_compatible_mode = 0,
#endif
        },
    };

    // Create the one-shot timer used for non-blocking WiFi reconnect backoff
    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = &reconnect_cb,
        .name     = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Modem sleep can drop unicast UDP replies (SNTP); TCP survives via
       retransmission, UDP does not. The radio is only on briefly per wake */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Wait for the first successful connection, but not forever: with the router
       off, an endless wait would keep the radio on and drain the battery.
       The reconnect timer keeps retrying in the background meanwhile */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(timeout_ms));     // Give up after timeout_ms

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "not connected within %" PRIu32 " ms", timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "connected to ap SSID: %s", WIFI_SSID);
    return ESP_OK;
}

/* Clean shutdown before deep sleep or a reboot. Releasing the association makes
   the next wake-up reconnect cleaner than simply cutting power to the radio */
void wifi_stop(void)
{
    s_stopping = true;                  // Suppress the reconnect attempt the stop below would trigger
    esp_timer_stop(s_reconnect_timer);  // Cancel a pending reconnect (error if not armed is harmless)
    esp_wifi_stop();
}