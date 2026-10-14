#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Production glue for post-OTA rollback confirmation (receiver only).
 *
 * Follows the ESP-IDF native_ota_example pattern: instead of confirming the
 * image straight from app_main, remember whether the running image is
 * ESP_OTA_IMG_PENDING_VERIFY, mark the dashboard up once httpd_start()
 * succeeded, and confirm only after the stability window. If the image never
 * reaches "dashboard up", roll back to the previous OTA slot. This replaces
 * the previous unconditional esp_ota_mark_app_valid_cancel_rollback() at boot,
 * which threw away the CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE safety net.
 *
 * Scope: receiver only. The sender has no dashboard, so its build compiles the
 * no-op stubs at the bottom of boot_confirm.c and keeps its existing behaviour.
 *
 * HARDWARE VERIFICATION OPEN: the logic is host-tested (test_boot_policy.c),
 * but the real OTA -> confirm / rollback path still has to be validated on a
 * device before this is considered proven.
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

#ifdef BOOT_CONFIRM_TEST_HOOKS
/* Test-only: drive a virtual uptime and observe the side effects, so the host
 * can assert that a failure scenario really triggers the rollback (the firmware
 * never calls these). */
#include "boot_policy.h"

void boot_confirm_test_reset(void);
void boot_confirm_test_set_uptime(uint32_t uptime_ms);
boot_effect_t boot_confirm_test_last_effect(void);
unsigned boot_confirm_test_fail_count(void);
const char *boot_confirm_test_fail_reason(void);
bool boot_confirm_test_confirmed(void);
#endif
