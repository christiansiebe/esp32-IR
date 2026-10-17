#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure, host-testable decision logic for post-OTA rollback confirmation.
 *
 * When a new image is flashed, the bootloader starts it in the
 * ESP_OTA_IMG_PENDING_VERIFY state and expects the application to confirm its
 * operability. Until it is confirmed, *any* reset (including a task-watchdog
 * panic) rolls the device back to the previous image
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). Confirming too early -- e.g. calling
 * esp_ota_mark_app_valid_cancel_rollback() straight from app_main -- throws
 * that safety net away: the device then keeps booting a broken image instead of
 * falling back.
 *
 * This module keeps the "keep this image?" decision pure so the error paths can
 * be simulated without a device (see test/test_boot_policy.c). The production
 * criteria are deliberately conservative: the image must stay up for a
 * stability window and the local dashboard must be reachable. Arrival of SML
 * meter data is intentionally NOT required, because an absent/silent sender
 * must never roll back an otherwise healthy image.
 */

typedef enum {
    BOOT_TAKE_NONE = 0,     /* nothing to do yet (keep polling) */
    BOOT_TAKE_CONFIRM = 1,  /* esp_ota_mark_app_valid_cancel_rollback() */
    BOOT_TAKE_ROLLBACK = 2, /* esp_ota_mark_app_invalid_rollback_and_reboot() */
} boot_action_t;

typedef struct {
    uint32_t stability_ms;  /* uptime after which a healthy image is confirmed */
    uint32_t boot_grace_ms; /* time the local subsystems get to come up first */
} boot_policy_t;

typedef struct {
    bool is_pending_verify; /* running image is awaiting confirmation */
    bool dashboard_ok;      /* local dashboard subsystem reached up */
} boot_inputs_t;

typedef struct {
    bool decided; /* a confirm/rollback decision has been made (final) */
} boot_state_t;

/* The device action a decided boot_action_t must trigger. Kept pure so the
 * failure paths can be checked end to end on the host: boot_confirm_poll() only
 * *performs* the effect, it does not decide it. This is exactly where the
 * rollback was previously dropped (BOOT_TAKE_ROLLBACK mapped to nothing). */
typedef enum {
    BOOT_EFFECT_NONE = 0,
    BOOT_EFFECT_CONFIRM = 1,  /* esp_ota_mark_app_valid_cancel_rollback() */
    BOOT_EFFECT_ROLLBACK = 2, /* boot_confirm_fail() -> rollback + reboot */
} boot_effect_t;

void boot_policy_defaults(boot_policy_t *policy);

void boot_state_reset(boot_state_t *state);

/* Map a decided action to the side effect the glue must perform. */
boot_effect_t boot_action_effect(boot_action_t action);

/* Evaluate once. `uptime_ms` is the time since boot. Returns the action to
 * take. BOOT_TAKE_ROLLBACK fires once the grace window passed and the
 * dashboard is still not up; BOOT_TAKE_CONFIRM fires once the stability window
 * passed with the dashboard up. After either decision the state is final. */
boot_action_t boot_evaluate(const boot_policy_t *policy, boot_state_t *state,
                            uint32_t uptime_ms, const boot_inputs_t *inputs);
