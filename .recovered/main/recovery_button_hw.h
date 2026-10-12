#pragma once

#include "esp_err.h"

/*
 * Startup recovery-button check (receiver). If the BOOT button is held for the
 * configured hold time, force a rollback to the previous firmware and reboot.
 * On a healthy boot this returns quickly (no button, or a short press).
 *
 * Must be called after the strapping-pin boot decision, i.e. once the
 * application is running.
 */
esp_err_t recovery_button_check(void);

/* Force the rollback immediately. Does not return on success; returns the
 * error if a rollback was not possible (e.g. no valid image in the other
 * slot). Exposed so the web console can offer the same escape hatch. */
esp_err_t recovery_button_force_rollback(void);
