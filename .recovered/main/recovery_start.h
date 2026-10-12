#pragma once

#include "esp_err.h"

/*
 * Startup recovery-button check. If the BOOT button (CONFIG_RECOVERY_BUTTON_GPIO)
 * is held for CONFIG_RECOVERY_BUTTON_HOLD_MS at startup, force a rollback to the
 * previous firmware and reboot. On a normal boot this returns quickly.
 */
esp_err_t recovery_button_check(void);

/* Force the rollback now. Does not return on success; returns the error when no
 * rollback is possible (no valid image in the other slot). */
esp_err_t recovery_button_force_rollback(void);
