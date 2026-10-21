#pragma once

#include "esp_err.h"

/*
 * Startup recovery-button check (receiver only).
 *
 * A single soldered BOOT-pin button (CONFIG_RECOVERY_BUTTON_GPIO, -1 disables)
 * drives two actions, told apart by how long it is held after boot:
 *
 *   - held >= CONFIG_RECOVERY_BUTTON_RESTORE_MS and released before the
 *     rollback window -> RECOVERY_RESTORE: the previous runtime-config values
 *     are put back (the NVS 'prev' slot) and the device reboots.
 *   - held >= CONFIG_RECOVERY_BUTTON_ROLLBACK_MS -> RECOVERY_ROLLBACK: force a
 *     rollback to the previous firmware, even when the running image was
 *     already confirmed. This is the manual escape hatch for "confirmed but
 *     broken", which the automatic post-OTA policy (boot_confirm.c) cannot
 *     cover.
 *
 * Must be called once the application is running, after the strapping-pin boot
 * decision (the ROM samples the pin while it is held for the download mode), and
 * BEFORE the persisted CPU knobs are applied -- a bad 'set cpu-min 40' hangs the
 * device in apply_power_management(), so the escape hatch has to run first.
 *
 * On a normal boot this returns quickly (button not pressed, or a short press).
 */

/* Register the callback that restores the previous runtime-config values and
 * reboots. Called on a RECOVERY_RESTORE gesture. The glue keeps no NVS
 * knowledge itself; main.c owns the two-slot store. */
typedef void (*recovery_restore_fn)(void);
void recovery_set_restore_fn(recovery_restore_fn fn);

/* Returns ESP_OK when the boot continues normally. On a restore it does not
 * return (it reboots); on a forced rollback it does not return either. It
 * returns an error only when the requested action could not be started. */
esp_err_t recovery_button_check(void);

/* Force the rollback immediately. Does not return on success; returns the
 * error when a rollback is not possible. Exposed so the web console can offer
 * the same escape hatch. */
esp_err_t recovery_button_force_rollback(void);
