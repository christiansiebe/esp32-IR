#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Production glue for post-OTA rollback confirmation (receiver).
 *
 * Follows the ESP-IDF native_ota_example pattern: instead of confirming the
 * image straight from app_main, remember whether the running image is
 * ESP_OTA_IMG_PENDING_VERIFY, mark the dashboard up once httpd_start()
 * succeeded, and confirm only after the stability window. If the image never
 * reaches "dashboard up", roll back. The decision itself lives in boot_policy.c
 * and is simulated on the host.
 */

/* Read the running image state. Call once, early in app_main. */
esp_err_t boot_confirm_init(void);

/* Record that the local dashboard subsystem came up. */
void boot_confirm_note_dashboard(bool ok);

/* Evaluate the policy once and perform confirm if it is due. Idempotent and
 * cheap; safe to call from any periodic loop. */
void boot_confirm_poll(void);

/* Handle a health failure: roll back if the image is still unconfirmed,
 * otherwise a plain reboot (the image was already accepted). Does not return. */
void boot_confirm_fail(const char *reason_name);
