#include "recovery_button.h"

void recovery_state_reset(recovery_state_t *state)
{
    state->active = false;
    state->hold_start_ms = 0;
}

recovery_action_t recovery_evaluate(recovery_state_t *state, uint32_t uptime_ms,
                                    bool pressed, uint32_t restore_ms,
                                    uint32_t rollback_ms)
{
    if (!pressed) {
        bool was_active = state->active;
        uint32_t held_ms =
            was_active ? (uint32_t)(uptime_ms - state->hold_start_ms) : 0u;
        state->active = false;
        if (was_active && held_ms >= restore_ms) {
            return RECOVERY_RESTORE;
        }
        return RECOVERY_IDLE;
    }

    if (!state->active) {
        state->active = true;
        state->hold_start_ms = uptime_ms;
        return RECOVERY_IDLE;
    }

    if ((uint32_t)(uptime_ms - state->hold_start_ms) >= rollback_ms) {
        return RECOVERY_ROLLBACK;
    }
    return RECOVERY_IDLE;
}
