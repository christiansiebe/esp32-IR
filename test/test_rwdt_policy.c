/* Host test for the RWDT backstop tick arithmetic (rwdt_policy.c).
 *
 * The hardware behaviour (a stage-0 timeout resetting the chip) cannot be
 * exercised on the host; what CAN be pinned down is the tick conversion,
 * because getting it wrong makes the watchdog fire far too early or far too
 * late. The critical property: the requested timeout is never shortened by the
 * implicit stage-0 multiplier, so the converted ticks always represent at
 * least the requested time. */
#include <stdio.h>

#include "rwdt_policy.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (!(cond)) {                                                      \
            ++failures;                                                     \
            printf("FAIL: %s\n", (msg));                                    \
        }                                                                   \
    } while (0)

int main(void)
{
    /* Clamping: below the floor, above the ceiling, and in range. */
    CHECK(rwdt_clamp_timeout_ms(0) == RWDT_MIN_TIMEOUT_MS, "0 clamps up to the floor");
    CHECK(rwdt_clamp_timeout_ms(1000) == RWDT_MIN_TIMEOUT_MS, "1 s clamps up to the floor");
    CHECK(rwdt_clamp_timeout_ms(RWDT_MIN_TIMEOUT_MS) == RWDT_MIN_TIMEOUT_MS, "floor stays");
    CHECK(rwdt_clamp_timeout_ms(180000) == 180000, "180 s is in range");
    CHECK(rwdt_clamp_timeout_ms(0xFFFFFFFFu) == RWDT_MAX_TIMEOUT_MS, "huge clamps to ceiling");

    /* A zero slow clock means "unknown": never arm. */
    CHECK(rwdt_timeout_ticks(180000, 0) == 0, "unknown slow clock yields 0 ticks");
    CHECK(rwdt_timeout_ticks(0, 32768) == 0, "zero timeout yields 0 ticks");

    /* 32.768 kHz crystal (the common slow clock): 180 s = 5,898,240 ticks,
     * already a multiple of 16, so rounding must not change it. */
    CHECK(rwdt_timeout_ticks(180000, 32768) == 5898240u, "180 s at 32768 Hz");

    /* 150 kHz RC_SLOW: 180 s = 27,000,000 ticks, multiple of 16 -> unchanged. */
    CHECK(rwdt_timeout_ticks(180000, 150000) == 27000000u, "180 s at 150 kHz");

    /* A value that is NOT a multiple of 16 must round UP, never down, so the
     * hardware's implicit multiplier cannot shorten the timeout below the
     * request. 10 ms at 32.768 kHz = 327 ticks -> rounds to 336. */
    uint32_t t = rwdt_timeout_ticks(10, 32768);
    CHECK(t == 336u, "unaligned rounds up to the next multiple of 16");
    CHECK(t % 16u == 0u, "result is always a multiple of 16");
    CHECK((uint64_t)t * 1000u / 32768u >= 10u, "effective time is not shorter than requested");

    /* Monotonic in the timeout: a larger request never yields fewer ticks. */
    CHECK(rwdt_timeout_ticks(200000, 32768) >= rwdt_timeout_ticks(180000, 32768),
          "larger timeout yields at least as many ticks");

    /* Very large input saturates rather than wrapping (a 1 GHz slow clock is
     * absurd, but the arithmetic must not silently wrap to a tiny timeout). */
    uint32_t big = rwdt_timeout_ticks(3600000, 1000000000u);
    CHECK(big == (0xFFFFFFFFu & ~15u), "saturates at the top without wrapping");

    if (failures == 0) {
        printf("test_rwdt_policy: %d checks passed\n", checks);
        return 0;
    }
    printf("test_rwdt_policy: %d/%d checks FAILED\n", failures, checks);
    return 1;
}
