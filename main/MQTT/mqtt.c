#include <stdio.h>
#include <stdbool.h>
#include "config.h"     // Pins definitions
#include "driver/gpio.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>     // strlen, memcmp, strlcpy

#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"      // JSON parser bundled with ESP-IDF

// --- USER .h FILES ---
#include "mqtt.h"
#include "secrets.h"    // AWS_IOT_ENDPOINT, AWS_IOT_CLIENT_ID

// Must match the iot:Publish resource in the AWS IoT policy
#define MQTT_TOPIC  "sensors/" AWS_IOT_CLIENT_ID "/data"

// AWS IoT Jobs reserved topics — thing name must equal AWS_IOT_CLIENT_ID
#define JOBS_PREFIX      "$aws/things/" AWS_IOT_CLIENT_ID "/jobs/"
#define JOBS_START_NEXT  JOBS_PREFIX "start-next"

static const char *TAG = "mqtt";

/* Certificates embedded via EMBED_TXTFILES (main/CMakeLists.txt)
   EMBED_TXTFILES NUL-terminates them, so no explicit length is needed */
extern const char aws_root_ca_pem[] asm("_binary_AmazonRootCA1_pem_start");   // Verifies the AWS server
extern const char device_cert_pem[] asm("_binary_device_pem_crt_start");      // Identifies this device
extern const char private_key_pem[] asm("_binary_private_pem_key_start");     // Proves it owns the cert

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;   // written by MQTT task, read by app_main
static volatile bool s_subscribed = false;    // SUBSCRIBED event arrived
static volatile bool s_job_answered = false;  // start-next answer arrived
static volatile int s_acked_msg_id = -1;      // msg_id of the last QoS 1 publish the broker confirmed
static bool s_job_found = false;
static mqtt_job_t s_job;                      // filled from the start-next answer

// True when the event's topic is exactly `topic` (event->topic is not NUL-terminated)
static bool topic_is(esp_mqtt_event_handle_t event, const char *topic)
{
    return event->topic_len == (int)strlen(topic) &&
           memcmp(event->topic, topic, event->topic_len) == 0;
}

// Pick jobId, version and url out of the start-next/accepted payload
static void parse_start_next(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    cJSON *execution = cJSON_GetObjectItem(root, "execution");  // missing = no pending job
    cJSON *id = cJSON_GetObjectItem(execution, "jobId");
    cJSON *doc = cJSON_GetObjectItem(execution, "jobDocument");
    cJSON *version = cJSON_GetObjectItem(doc, "version");
    cJSON *url = cJSON_GetObjectItem(doc, "url");

    s_job_found = cJSON_IsString(id) && cJSON_IsString(version) && cJSON_IsString(url) &&
                  strlen(url->valuestring) < sizeof(s_job.url);
    if (s_job_found) {
        strlcpy(s_job.id, id->valuestring, sizeof(s_job.id));
        strlcpy(s_job.version, version->valuestring, sizeof(s_job.version));
        strlcpy(s_job.url, url->valuestring, sizeof(s_job.url));
    }
    cJSON_Delete(root);     // free the memory cJSON allocated
}

// Poll a flag set by the MQTT task, give up after timeout_ms
static esp_err_t wait_for_flag(const volatile bool *flag, uint32_t timeout_ms)
{
    const TickType_t step = pdMS_TO_TICKS(100);
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    TickType_t waited = 0;

    while (!*flag) {
        if (waited >= limit) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(step);
        waited += step;
    }
    return ESP_OK;
}

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
    case MQTT_EVENT_SUBSCRIBED:
        s_subscribed = true;
        break;
    case MQTT_EVENT_PUBLISHED: {
        esp_mqtt_event_handle_t event = event_data;
        s_acked_msg_id = event->msg_id;     // broker confirmed a QoS 1 publish
        break;
    }
    case MQTT_EVENT_DATA: {
        esp_mqtt_event_handle_t event = event_data;
        if (event->data_len != event->total_data_len) {
            ESP_LOGE(TAG, "Message too big for MQTT buffer (%d bytes)", event->total_data_len);
            break;
        }
        if (topic_is(event, JOBS_START_NEXT "/accepted")) {
            parse_start_next(event->data, event->data_len);
            s_job_answered = true;
        } else if (topic_is(event, JOBS_START_NEXT "/rejected")) {
            ESP_LOGW(TAG, "Job request rejected: %.*s", event->data_len, event->data);
            s_job_answered = true;
        }
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
        .buffer.size = 4096,    // start-next answer carries a ~1-2 kB presigned URL
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
    return wait_for_flag(&s_connected, timeout_ms);
}

/* Ask AWS IoT Jobs for the next pending job of this thing
   AWS answers on start-next/accepted: with "execution" when a job is waiting, without it otherwise */
const mqtt_job_t *mqtt_get_next_job(uint32_t timeout_ms)
{
    if (s_client == NULL || !s_connected) {
        return NULL;
    }

    // 1. Listen for both answers (accepted / rejected) before asking
    s_subscribed = false;
    if (esp_mqtt_client_subscribe_single(s_client, JOBS_START_NEXT "/+", 1) < 0 ||
        wait_for_flag(&s_subscribed, timeout_ms) != ESP_OK) {
        ESP_LOGW(TAG, "Subscribe to job topics failed");
        return NULL;
    }

    // 2. Ask for the next job — "{}" means no extra parameters
    s_job_answered = false;
    s_job_found = false;
    if (esp_mqtt_client_publish(s_client, JOBS_START_NEXT, "{}", 0, 1, 0) < 0 ||
        wait_for_flag(&s_job_answered, timeout_ms) != ESP_OK) {
        ESP_LOGW(TAG, "No answer from AWS IoT Jobs");
        return NULL;
    }

    if (!s_job_found) {
        ESP_LOGI(TAG, "No pending job");
        return NULL;
    }
    ESP_LOGI(TAG, "Job %s: version %s", s_job.id, s_job.version);
    return &s_job;
}

// Report the job result to AWS and wait for the broker to confirm it (QoS 1)
esp_err_t mqtt_update_job_status(const char *job_id, const char *status, uint32_t timeout_ms)
{
    if (s_client == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    char payload[32];
    snprintf(topic, sizeof(topic), JOBS_PREFIX "%s/update", job_id);
    snprintf(payload, sizeof(payload), "{\"status\":\"%s\"}", status);

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }

    /* Without this wait a reboot or deep sleep right after could cut the message off
       and the job would stay IN_PROGRESS in AWS */
    const TickType_t step = pdMS_TO_TICKS(100);
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    for (TickType_t waited = 0; s_acked_msg_id != msg_id; waited += step) {
        if (waited >= limit) {
            ESP_LOGW(TAG, "Job %s status %s not confirmed", job_id, status);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(step);
    }
    ESP_LOGI(TAG, "Job %s -> %s", job_id, status);
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
