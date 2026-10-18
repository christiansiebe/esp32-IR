/* Host tests for the pure health/watchdog policy. No ESP-IDF.
 *
 * The receiver reboots itself when the meter data has been silent for too long
 * or the heap fell below the floor. That decision is what must be exact, so the
 * grace window, the staleness limit, the heap floor and the latch are locked
 * down here. The IDF glue (health_task.c) only performs the reboot.
 */
#include <stdio.h>
#include <string.h>

#include "health_policy.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

static void test_grace_period(void)
{
    health_policy_t policy;
    health_policy_defaults(&policy);
    health_state_t state;
    health_state_reset(&state);

    /* Within the grace period nothing may fire, even with no data ever. */
    CHECK(health_evaluate(&policy, &state, 60 * 1000, 0, 100000) == HEALTH_OK,
          "no reboot inside grace period");
    /* Exactly at the grace boundary the rules become active (nothing to fire
     * on here, but the low heap must be seen from this tick on). */
    CHECK(health_evaluate(&policy, &state, policy.grace_ms, 0, 100000) == HEALTH_OK,
          "grace boundary with healthy inputs is ok");
    CHECK(health_evaluate(&policy, &state, policy.grace_ms, 0, 1) == HEALTH_LOW_HEAP,
          "grace boundary with low heap fires");
}

static void test_stale_data_fires(void)
{
    health_policy_t policy;
    health_policy_defaults(&policy);
    health_state_t state;
    health_state_reset(&state);

    uint32_t now = policy.grace_ms + 1000;
    uint32_t last_rx = now - 1000; /* fresh */
    CHECK(health_evaluate(&policy, &state, now, last_rx, 100000) == HEALTH_OK,
          "fresh data is healthy");

    /* Exactly at the staleness limit is not yet stale (the limit is exclusive). */
    last_rx = now - policy.stale_data_ms;
    CHECK(health_evaluate(&policy, &state, now, last_rx, 100000) == HEALTH_OK,
          "exactly at the limit is not stale");

    /* Data stopped long ago. */
    last_rx = now - policy.stale_data_ms - 1;
    CHECK(health_evaluate(&policy, &state, now, last_rx, 100000) == HEALTH_STALE_DATA,
          "stale data fires");
    CHECK(state.latched, "stale data latches");
    CHECK(strcmp(health_reason_name(state.reason), "stale-data") == 0, "reason name");

    /* Stays latched even if data returns before the reboot happens. */
    CHECK(health_evaluate(&policy, &state, now + 1000, now + 1000, 100000) ==
              HEALTH_STALE_DATA,
          "latched reason is sticky");
}

static void test_never_received_is_not_stale(void)
{
    health_policy_t policy;
    health_policy_defaults(&policy);
    health_state_t state;
    health_state_reset(&state);

    /* last_rx == 0 means "no value yet": do not reboot, the device may simply
     * have no sender in range during commissioning. */
    CHECK(health_evaluate(&policy, &state, policy.grace_ms + 60 * 60 * 1000, 0,
                          100000) == HEALTH_OK,
          "no data yet is not treated as stale");
}

static void test_low_heap_fires(void)
{
    health_policy_t policy;
    health_policy_defaults(&policy);
    health_state_t state;
    health_state_reset(&state);

    CHECK(health_evaluate(&policy, &state, policy.grace_ms + 1000,
                          policy.grace_ms, 100000) == HEALTH_OK, "healthy heap");
    CHECK(health_evaluate(&policy, &state, policy.grace_ms + 2000,
                          policy.grace_ms, 100) == HEALTH_LOW_HEAP, "low heap fires");
    CHECK(strcmp(health_reason_name(state.reason), "low-heap") == 0, "low heap name");
}

static void test_disabled_rules(void)
{
    health_policy_t policy = {0}; /* everything disabled */
    health_state_t state;
    health_state_reset(&state);
    CHECK(health_evaluate(&policy, &state, 10 * 60 * 1000, 1, 1) == HEALTH_OK,
          "disabled rules never fire");
}

static void test_reset_clears_latch(void)
{
    health_policy_t policy;
    health_policy_defaults(&policy);
    health_state_t state;
    health_state_reset(&state);

    uint32_t now = policy.grace_ms + policy.stale_data_ms + 1;
    CHECK(health_evaluate(&policy, &state, now, 1, 100000) == HEALTH_STALE_DATA,
          "stale data fires before reset");
    health_state_reset(&state);
    CHECK(!state.latched, "reset clears the latch");
    CHECK(state.reason == HEALTH_OK, "reset clears the reason");
    CHECK(health_evaluate(&policy, &state, now, now, 100000) == HEALTH_OK,
          "fresh data is healthy again after a reset");
}

int main(void)
{
    test_grace_period();
    test_stale_data_fires();
    test_never_received_is_not_stale();
    test_low_heap_fires();
    test_disabled_rules();
    test_reset_clears_latch();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
