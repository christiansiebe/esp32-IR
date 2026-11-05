#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure, host-testable evaluation of the startup recovery-button gesture.
 *
 * The board carries a soldered button on the BOOT pin (GPIO0 on most devkits).
 * That pin is a strapping pin: it is sampled by the ROM bootloader and, while
 * held, requests the UART download mode. We therefore only start watching it
 * *after* boot has continued. One button drives two actions, separated by how
 * long it is held:
 *
 *   - hold >= restore_ms and release before rollback_ms -> RECOVERY_RESTORE
 *     (put the previous runtime-config values back, then reboot)
 *   - hold >= rollback_ms (still held)                  -> RECOVERY_ROLLBACK
 *     (force a rollback to the previous firmware, even when the running image
 *      was already confirmed -- the escape hatch for "confirmed but broken")
 *
 * The restore fires on *release*, not while held: otherwise a 3 s hold would
 * reboot the device before the 10 s rollback could ever be reached. The button
 * level is fed in as a plain bool so the timing logic is unit-testable without
 * hardware (see test/test_recovery_button.c).
 */

typedef enum {
    RECOVERY_IDLE = 0,     /* nothing to do yet */
    RECOVERY_RESTORE = 1,  /* released after >= restore_ms: restore config */
    RECOVERY_ROLLBACK = 2, /* held >= rollback_ms: force a firmware rollback */
} recovery_action_t;

typedef struct {
    bool active;            /* a hold is being timed */
    uint32_t hold_start_ms; /* uptime the current hold began */
} recovery_state_t;

void recovery_state_reset(recovery_state_t *state);

/* Feed the debounced button level (true = pressed) and the current uptime.
 * Returns RECOVERY_ROLLBACK once the button has been held continuously for
 * `rollback_ms`, or RECOVERY_RESTORE when it is released after a hold of at
 * least `restore_ms` but less than `rollback_ms`. A shorter press is abandoned
 * and returns RECOVERY_IDLE. The hold timer restarts after any release. */
recovery_action_t recovery_evaluate(recovery_state_t *state, uint32_t uptime_ms,
                                    bool pressed, uint32_t restore_ms,
                                    uint32_t rollback_ms);
