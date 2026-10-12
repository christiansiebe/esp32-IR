#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Pure, host-testable evaluation of the startup recovery-button gesture.
 *
 * The recovery button (the on-board BOOT button, GPIO0 on most devkits) is a
 * strapping pin: it is sampled by the ROM bootloader and, while held, requests
 * the UART download mode. We therefore only start watching it *after* boot has
 * continued, and require it to be held continuously for a hold window. The
 * gesture forces a rollback to the previous firmware, even when the running
 * image was already confirmed -- the manual escape hatch for "confirmed but
 * broken". The button level is fed in as a plain bool so the timing logic is
 * unit-testable without hardware (see test/test_recovery_button.c).
 */

typedef enum {
    RECOVERY_IDLE = 0,     /* nothing to do yet */
    RECOVERY_FORCE = 1,    /* held long enough: force a rollback */
} recovery_action_t;

typedef struct {
    bool active;             /* a hold is being timed */
    uint32_t hold_start_ms;  /* uptime the current hold began */
} recovery_state_t;

void recovery_state_reset(recovery_state_t *state);

/* Feed the debounced button level (true = pressed) and the current uptime.
 * Returns RECOVERY_FORCE once the button has been held continuously for
 * `hold_ms`; releasing the button before that resets the timer. */
recovery_action_t recovery_evaluate(recovery_state_t *state, uint32_t uptime_ms,
                                    bool pressed, uint32_t hold_ms);
