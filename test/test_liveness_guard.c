/* Host test for the global liveness watchdog state machine. */
#include <stdio.h>

#include "liveness_guard.h"

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
    liveness_policy_t policy;
    liveness_default_policy(&policy);
    CHECK(policy.feed_timeout_ms == 60000, "default feed timeout is 60 s");
    CHECK(policy.panic_timeout_ms == 30000, "default panic timeout is 30 s");

    /* A short policy so the test reads clearly. The stages are absolute
     * silences from the last feed: warn 1000, panic 1500, reboot 2000 ms. */
    policy.feed_timeout_ms = 1000;
    policy.panic_timeout_ms = 500;
    policy.reboot_timeout_ms = 500;
    policy.min_uptime_ms = 2000;

    liveness_guard_t g;
    liveness_init(&g, &policy, 10000);

    /* Grace: nothing fires inside min_uptime, even with no feed. */
    CHECK(liveness_tick(&g, 11000) == LIVENESS_OK, "silent inside grace");
    CHECK(liveness_tick(&g, 11999) == LIVENESS_OK, "silent at grace edge");

    /* Feed once the grace has passed, then measure from there. */
    liveness_feed(&g, 12000);
    CHECK(liveness_silence_ms(&g, 12000) == 0, "feed resets silence");
    CHECK(liveness_tick(&g, 12000) == LIVENESS_OK, "quiet right after feed");
    CHECK(liveness_tick(&g, 12999) == LIVENESS_OK, "below feed timeout");

    CHECK(liveness_tick(&g, 13000) == LIVENESS_WARN, "first silence warns");
    CHECK(liveness_tick(&g, 13001) == LIVENESS_OK, "warn is latched");

    CHECK(liveness_tick(&g, 13499) == LIVENESS_OK, "below panic timeout");
    CHECK(liveness_tick(&g, 13500) == LIVENESS_PANIC, "longer silence panics");
    CHECK(liveness_tick(&g, 13501) == LIVENESS_OK, "panic is latched");

    CHECK(liveness_tick(&g, 13999) == LIVENESS_OK, "below reboot timeout");
    CHECK(liveness_tick(&g, 14000) == LIVENESS_REBOOT, "reboot stage");
    CHECK(liveness_tick(&g, 14001) == LIVENESS_REBOOT, "reboot repeats");

    /* A feed clears every stage and restarts the countdown. */
    liveness_feed(&g, 14100);
    CHECK(liveness_silence_ms(&g, 14100) == 0, "feed resets again");
    CHECK(liveness_tick(&g, 14500) == LIVENESS_OK, "quiet after feed");
    CHECK(liveness_tick(&g, 15100) == LIVENESS_WARN, "warns again after feed");

    /* Steady feeding never trips anything. */
    liveness_guard_t h;
    liveness_init(&h, &policy, 0);
    for (uint64_t t = 0; t < 60000; t += 500) {
        liveness_feed(&h, t);
        CHECK(liveness_tick(&h, t) == LIVENESS_OK, "steady feed stays OK");
    }

    /* A clock that jumps backwards must not fire. */
    liveness_guard_t b;
    liveness_init(&b, &policy, 100000);
    liveness_feed(&b, 100000);
    CHECK(liveness_tick(&b, 90000) == LIVENESS_OK, "backwards clock is safe");

    printf("liveness guard: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
