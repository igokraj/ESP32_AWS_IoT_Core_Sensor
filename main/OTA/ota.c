#include "esp_log.h"
#include "esp_system.h"     // esp_restart
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"   // esp_app_get_description, esp_app_desc_t
#include <string.h>
#include "esp_ota_ops.h"    // Rollback functions
#include "esp_crt_bundle.h"
#include "ota.h"
#include "nvs.h"            // persist the "last known-bad version" across deep sleep

static const char *TAG = "ota";



/* Where the known-bad version is stored. NVS rather than a global or RTC memory:
   it has to survive deep sleep and a full power loss, or a broken version could
   be downloaded and fail all over again on the next wake */
#define NVS_NAMESPACE    "ota"
#define NVS_KEY_BAD_VER  "bad_ver"



// Read the last version that failed diagnostics (empty string if none recorded)
static void get_blacklisted_version(char *out, size_t out_size)
{
    out[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = out_size;
        nvs_get_str(nvs, NVS_KEY_BAD_VER, out, &len);   // leaves out="" if key not found
        nvs_close(nvs);
    }
}

// Remember this version as known-bad, so we stop re-downloading it every wake
static void blacklist_version(const char *version)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_BAD_VER, version);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// True when this version was installed before and failed diagnostics
bool ota_is_version_blacklisted(const char *version)
{
    char bad_version[32];   // same size as esp_app_desc_t.version
    get_blacklisted_version(bad_version, sizeof(bad_version));
    return bad_version[0] != '\0' && strcmp(version, bad_version) == 0;
}

// Ask the server whether a newer image exists and install it if so
ota_result_t ota_check_and_update(const char *url)
{
    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size_tx = 4096,     // presigned S3 URL is ~1-2 kB, default 512 is too small
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    // 1. Open the connection and fetch just the image header
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        // S3 unreachable, or the presigned URL expired / was refused (HTTP 403)
        ESP_LOGW(TAG, "Cannot start download (%s)", esp_err_to_name(err));
        return OTA_RESULT_ERROR;
    }

    // 2. Read the version string out of the downloaded header
    esp_app_desc_t new_app;
    err = esp_https_ota_get_img_desc(handle, &new_app);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read image description");
        esp_https_ota_abort(handle);
        return OTA_RESULT_ERROR;
    }

    // 2b. Skip a version we already know is broken — no point retrying it forever
    if (ota_is_version_blacklisted(new_app.version)) {
        ESP_LOGW(TAG, "Version %s previously failed diagnostics — skipping", new_app.version);
        esp_https_ota_abort(handle);
        return OTA_RESULT_NO_UPDATE;
    }

    // 3. Compare with the version we are currently running
    const esp_app_desc_t *running = esp_app_get_description();
    ESP_LOGI(TAG, "running=%s  available=%s", running->version, new_app.version);
    if (strcmp(new_app.version, running->version) == 0) {
        ESP_LOGI(TAG, "Already up to date — skipping");
        esp_https_ota_abort(handle);    // stop the download
        return OTA_RESULT_NO_UPDATE;
    }

    /* 4. Different version — download the rest of the image, block by block
       The loop body is empty on purpose: perform() writes one chunk per call and
       keeps returning IN_PROGRESS until the whole image has been received */
    ESP_LOGI(TAG, "New version, downloading...");
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    }

    // 5. Did the whole image actually arrive?
    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "Incomplete image");
        esp_https_ota_abort(handle);
        return OTA_RESULT_ERROR;
    }

    // 6. Validate the image + mark the new slot as the one to boot next
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed (%s)", esp_err_to_name(err));
        return OTA_RESULT_ERROR;
    }

    ESP_LOGI(TAG, "Update installed, rebooting");
    return OTA_RESULT_UPDATE_READY;
}

// Confirm a freshly OTA-delivered image, or mark it for rollback
ota_diagnostic_result_t ota_confirm_or_rollback(bool diagnostic_is_ok)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (diagnostic_is_ok) {
                ESP_LOGI(TAG, "Diagnostics completed successfully! Continuing execution ...");
                esp_ota_mark_app_valid_cancel_rollback();
            } else {
                ESP_LOGE(TAG, "Diagnostics failed! Will roll back to the previous version ...");
                blacklist_version(esp_app_get_description()->version);
                return OTA_DIAGNOSTIC_FAILED;   // don't reboot yet — let caller clean up first
            }
        }
    }
    return OTA_DIAGNOSTIC_OK;
}

/* Discard the running image and boot back into the previous one
   Call only after mqtt_stop() and wifi_stop() — this never returns */
void ota_rollback_and_reboot(void)
{
    esp_ota_mark_app_invalid_rollback_and_reboot();     // never returns
}
