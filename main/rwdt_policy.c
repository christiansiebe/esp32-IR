#include "rwdt_policy.h"

uint32_t rwdt_clamp_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms < RWDT_MIN_TIMEOUT_MS) {
        return RWDT_MIN_TIMEOUT_MS;
    }
    if (timeout_ms > RWDT_MAX_TIMEOUT_MS) {
        return RWDT_MAX_TIMEOUT_MS;
    }
    return timeout_ms;
}

uint32_t rwdt_timeout_ticks(uint32_t timeout_ms, uint32_t slow_clk_hz)
{
    if (timeout_ms == 0 || slow_clk_hz == 0) {
        return 0;
    }
    uint64_t ticks = ((uint64_t)timeout_ms * (uint64_t)slow_clk_hz) / 1000u;
    /* Round up to the next multiple of 16 so the implicit stage-0 multiplier
     * never makes the effective timeout shorter than requested. */
    ticks = (ticks + 15u) & ~(uint64_t)15u;
    if (ticks > 0xFFFFFFFFu) {
        ticks = 0xFFFFFFFFu & ~(uint64_t)15u;
    }
    return (uint32_t)ticks;
}
