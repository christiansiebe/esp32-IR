#include "health_policy.h"

void health_policy_defaults(health_policy_t *policy)
{
    policy->stale_data_ms = 15u * 60u * 1000u; /* 15 minutes without a value */
    policy->low_heap_bytes = 12u * 1024u;      /* 12 KB free heap floor */
    policy->grace_ms = 3u * 60u * 1000u;       /* never reboot in the first 3 min */
}

void health_state_reset(health_state_t *state)
{
    state->latched = false;
    state->reason = HEALTH_OK;
}

health_reason_t health_evaluate(const health_policy_t *policy,
                                health_state_t *state, uint32_t uptime_ms,
                                uint32_t last_rx_ms, uint32_t free_heap)
{
    if (state->latched) {
        return state->reason;
    }
    if (uptime_ms < policy->grace_ms) {
        return HEALTH_OK;
    }

    if (policy->stale_data_ms != 0 && last_rx_ms != 0 &&
        (uint32_t)(uptime_ms - last_rx_ms) > policy->stale_data_ms) {
        state->latched = true;
        state->reason = HEALTH_STALE_DATA;
        return state->reason;
    }

    if (policy->low_heap_bytes != 0 && free_heap < policy->low_heap_bytes) {
        state->latched = true;
        state->reason = HEALTH_LOW_HEAP;
        return state->reason;
    }

    return HEALTH_OK;
}

const char *health_reason_name(health_reason_t reason)
{
    switch (reason) {
    case HEALTH_STALE_DATA: return "stale-data";
    case HEALTH_LOW_HEAP:   return "low-heap";
    case HEALTH_OK:
    default:                return "ok";
    }
}
