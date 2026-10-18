#pragma once

#include "esp_err.h"

/*
 * Startup recovery-button check (receiver only).
 *
 * If the BOOT button (CONFIG_RECOVERY_BUTTON_GPIO, -1 disables) is held for
 * CONFIG_RECOVERY_BUTTON_HOLD_MS at startup, force a rollback to the previous
 * firmware and reboot -- even when the running image was already confirmed.
 * This is the manual escape hatch for "confirmed but broken", which the
 * automatic post-OTA policy (boot_confirm.c) cannot cover. On a normal boot
 * this returns quickly (button not pressed, or a short press).
 *
 * Must be called once the application is running, after the strapping-pin boot
 * decision (the ROM samples the pin while it is held for the download mode).
 */

/* Returns ESP_OK when the boot continues normally. On a forced rollback this
 * does not return; it returns an error only when no rollback was possible
 * (e.g. no valid image in the other slot). */
esp_err_t recovery_button_check(void);

/* Force the rollback immediately. Does not return on success; returns the
 * error when a rollback is not possible. Exposed so the web console can offer
 * the same escape hatch. */
esp_err_t recovery_button_force_rollback(void);
