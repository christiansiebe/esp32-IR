#include "recovery_button.h"

void recovery_state_reset(recovery_state_t *state)
{
    state->active = false;
    state->hold_start_ms = 0;
}

recovery_action_t recovery_evaluate(recovery_state_t *state, uint32_t uptime_ms,
                                    bool pressed, uint32_t hold_ms)
{
    if (!pressed) {
        /* Released: any partial hold is abandoned. */
        state->active = false;
        return RECOVERY_IDLE;
    }

    if (!state->active) {
        state->active = true;
        state->hold_start_ms = uptime_ms;
        return RECOVERY_IDLE;
    }

    if ((uint32_t)(uptime_ms - state->hold_start_ms) >= hold_ms) {
        return RECOVERY_FORCE;
    }
    return RECOVERY_IDLE;
}
