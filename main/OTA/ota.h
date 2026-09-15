#pragma once    // include this file only once

#include "esp_err.h"
#include <stdbool.h>    // bool used in ota_confirm_or_rollback()

typedef enum {
    OTA_RESULT_NO_UPDATE,       // found no update available
    OTA_RESULT_UPDATE_READY,    // found update available
    OTA_RESULT_ERROR,           // there is an error
} ota_result_t;

/* Check the OTA server for a new update and install it if found
   Never reboots by itself — main.c decides when to restart */
ota_result_t ota_check_and_update(const char *url);

typedef enum {
    OTA_DIAGNOSTIC_OK,          // healthy, or nothing to confirm
    OTA_DIAGNOSTIC_FAILED,      // caller must shut down cleanly, then call ota_rollback_and_reboot()
} ota_diagnostic_result_t;

/* Confirm a freshly OTA-delivered image, or mark it for rollback
   Silently does nothing for a USB-flashed image — that never enters PENDING_VERIFY */
ota_diagnostic_result_t ota_confirm_or_rollback(bool diagnostic_is_ok);

/* Discard the running image and boot back into the previous one
   Call only after mqtt_stop() and wifi_stop() */
void ota_rollback_and_reboot(void);     // never returns
