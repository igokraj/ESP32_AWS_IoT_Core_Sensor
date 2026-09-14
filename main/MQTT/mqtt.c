#include <stdio.h>
#include <stdbool.h>
#include "config.h"     // Pins definitions
#include "driver/gpio.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "mqtt_client.h"

// --- USER .h FILES ---
#include "mqtt.h"
#include "secrets.h"    // AWS_IOT_ENDPOINT, AWS_IOT_CLIENT_ID

// Must match the iot:Publish resource in the AWS IoT policy
#define MQTT_TOPIC  "sensors/" AWS_IOT_CLIENT_ID "/data"

static const char *TAG = "mqtt";

/* Certificates embedded via EMBED_TXTFILES (main/CMakeLists.txt)
   EMBED_TXTFILES NUL-terminates them, so no explicit length is needed */
extern const char aws_root_ca_pem[] asm("_binary_AmazonRootCA1_pem_start");   // Verifies the AWS server
extern const char device_cert_pem[] asm("_binary_device_pem_crt_start");      // Identifies this device
extern const char private_key_pem[] asm("_binary_private_pem_key_start");     // Proves it owns the cert

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;   // written by MQTT task, read by app_main

/* Runs in the context of the MQTT client task
   Keep it short: update state, log — no blocking calls */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Connected to broker");
        gpio_set_level(MQTT_LED_PIN, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Disconnected, client will retry automatically");
        gpio_set_level(MQTT_LED_PIN, 0);
        break;
    case MQTT_EVENT_ERROR: {
        /* AWS rejects a policy mismatch (client ID / topic) by simply closing
           the connection, so the details below are the main debugging clue */
        esp_mqtt_event_handle_t event = event_data;
        ESP_LOGE(TAG, "MQTT error: type %d, esp-tls 0x%x, tls stack 0x%x, errno %d",
                 event->error_handle->error_type,
                 event->error_handle->esp_tls_last_esp_err,
                 event->error_handle->esp_tls_stack_err,
                 event->error_handle->esp_transport_sock_errno);
        gpio_set_level(MQTT_LED_PIN, 0);
        break;
    }
    default:
        break;
    }
}

/* Configure and start the client. Connecting happens asynchronously in the
   background, so this returns long before the broker is actually reachable
   mqtts:// selects TLS on port 8883: AWS verifies the device certificate,
   the device verifies AWS against the embedded Amazon Root CA */
void mqtt_start(void)
{
    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = "mqtts://" AWS_IOT_ENDPOINT ":8883",
        .broker.verification.certificate = aws_root_ca_pem,
        .credentials.client_id = AWS_IOT_CLIENT_ID,     // Must match iot:Connect in the policy
        .credentials.authentication.certificate = device_cert_pem,
        .credentials.authentication.key = private_key_pem,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Client init failed");
        return;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
}

// Format the reading as a JSON payload and publish it
esp_err_t mqtt_publish_reading(float temperature, float humidity)
{
    if (s_client == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;   // No broker: drop this reading
    }

    char json_payload[48];
    int len = snprintf(json_payload, sizeof(json_payload),
                       "{\"t\":%.2f,\"h\":%.2f}", temperature, humidity);

    // len >= sizeof means the payload was truncated
    if (len < 0 || len >= (int)sizeof(json_payload)) {
        return ESP_FAIL;
    }

    // QoS 0, no retain: fire-and-forget, a fresh reading follows shortly anyway
    int msg_id = esp_mqtt_client_publish(s_client, MQTT_TOPIC, json_payload, len, 0, 0);
    return (msg_id < 0) ? ESP_FAIL : ESP_OK;
}

/* Poll until the broker is connected, or give up after timeout_ms
   Needed because mqtt_start() connects asynchronously while deep sleep gives
   only one shot at publishing — without this wait the publish would be skipped */
esp_err_t mqtt_wait_for_connection(uint32_t timeout_ms)
{
    const TickType_t step = pdMS_TO_TICKS(100);
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    TickType_t waited = 0;

    while (!s_connected) {
        if (waited >= limit) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(step);
        waited += step;
    }
    return ESP_OK;
}

/* Clean shutdown before deep sleep or a reboot. Closing the TLS/TCP connection
   politely keeps the broker from logging an abrupt drop, and keeps transport errors
   out of the next boot log */
void mqtt_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
    }
}
