/* Host simulator for the post-OTA rollback decision. No ESP-IDF.
 *
 * Replays boot scenarios tick by tick and asserts which final action the
 * device would take. This is the "simulate the failure case" harness: it walks
 * a monotonic clock and reports confirm / rollback / nothing. */
#include <stdio.h>
#include <string.h>

#include "boot_policy.h"

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

/* Run a scenario: start at uptime 0, step 1 s at a time until 5 minutes.
 * `dashboard_up_at_ms` = when the dashboard becomes reachable (0 = never).
 * Returns the first non-NONE action, or NONE if the run ended undecided.
 * `ticks_to_decide` (if not NULL) receives the uptime at which it decided. */
static boot_action_t simulate(uint32_t dashboard_up_at_ms,
                              uint32_t *ticks_to_decide)
{
    boot_policy_t policy;
    boot_policy_defaults(&policy);
    boot_state_t state;
    boot_state_reset(&state);

    for (uint32_t t = 0; t <= 300000u; t += 1000u) {
        boot_inputs_t in = {
            .is_pending_verify = true,
            .dashboard_ok = (dashboard_up_at_ms != 0 && t >= dashboard_up_at_ms),
        };
        boot_action_t a = boot_evaluate(&policy, &state, t, &in);
        if (a != BOOT_TAKE_NONE) {
            if (ticks_to_decide) {
                *ticks_to_decide = t;
            }
            return a;
        }
    }
    return BOOT_TAKE_NONE;
}

static const char *action_name(boot_action_t a)
{
    switch (a) {
    case BOOT_TAKE_CONFIRM:  return "CONFIRM (keep image)";
    case BOOT_TAKE_ROLLBACK: return "ROLLBACK (previous image)";
    case BOOT_TAKE_NONE:
    default:                 return "pending";
    }
}

/* Print the decision timeline for a scenario so the run is observable. */
static void trace(const char *label, uint32_t dashboard_up_at_ms)
{
    boot_policy_t policy;
    boot_policy_defaults(&policy);
    boot_state_t state;
    boot_state_reset(&state);
    printf("  trace %-22s", label);
    for (uint32_t t = 0; t <= 300000u; t += 1000u) {
        boot_inputs_t in = {
            .is_pending_verify = true,
            .dashboard_ok = (dashboard_up_at_ms != 0 && t >= dashboard_up_at_ms),
        };
        boot_action_t a = boot_evaluate(&policy, &state, t, &in);
        if (a != BOOT_TAKE_NONE) {
            printf("-> at %u.%03us: %s\n", t / 1000u, t % 1000u, action_name(a));
            return;
        }
    }
    printf("-> still pending after 300s\n");
}

static void test_healthy_image_confirms(void)
{
    uint32_t at = 0;
    boot_action_t a = simulate(2000, &at); /* dashboard up after 2 s */
    CHECK(a == BOOT_TAKE_CONFIRM, "healthy image is confirmed");
    CHECK(at >= 45000u, "confirmation waits for the stability window");
}

static void test_broken_image_rolls_back(void)
{
    /* Dashboard subsystem never comes up: the classic "boots but unusable". */
    uint32_t at = 0;
    boot_action_t a = simulate(0, &at);
    CHECK(a == BOOT_TAKE_ROLLBACK, "unusable image rolls back");
    CHECK(at >= 30000u, "rollback waits for the boot grace window");
    CHECK(at < 45000u, "rollback happens before any confirmation");
}

static void test_late_dashboard_still_rolls_back(void)
{
    /* Dashboard arrives only after the grace deadline: the image already had
     * its one chance, so the device rolls back. */
    boot_action_t a = simulate(60000, NULL);
    CHECK(a == BOOT_TAKE_ROLLBACK, "too-late dashboard still rolls back");
}

static void test_no_pending_means_steady_state(void)
{
    /* After a confirmed boot the image is PENDING_VERIFY=false: normal resets
     * must never trigger a rollback. */
    boot_policy_t policy;
    boot_policy_defaults(&policy);
    boot_state_t state;
    boot_state_reset(&state);
    boot_inputs_t in = {.is_pending_verify = false, .dashboard_ok = false};
    CHECK(boot_evaluate(&policy, &state, 1000, &in) == BOOT_TAKE_NONE,
          "confirmed boot does nothing");
    CHECK(boot_evaluate(&policy, &state, 200000, &in) == BOOT_TAKE_NONE,
          "confirmed boot never rolls back");
}

static void test_decision_is_final(void)
{
    boot_policy_t policy;
    boot_policy_defaults(&policy);
    boot_state_t state;
    boot_state_reset(&state);
    boot_inputs_t in = {.is_pending_verify = true, .dashboard_ok = true};
    CHECK(boot_evaluate(&policy, &state, 50000, &in) == BOOT_TAKE_CONFIRM,
          "confirm at stability window");
    /* Once decided, further polls are silent (no repeat, no flap). */
    CHECK(boot_evaluate(&policy, &state, 60000, &in) == BOOT_TAKE_NONE,
          "no action after decision");
}

int main(void)
{
    printf("boot-policy simulator\n");
    printf("device powered on with a freshly flashed OTA image\n");
    trace("healthy (dash 2s)", 2000);
    trace("unusable (no dash)", 0);
    trace("late dash (60s)", 60000);
    test_healthy_image_confirms();
    test_broken_image_rolls_back();
    test_late_dashboard_still_rolls_back();
    test_no_pending_means_steady_state();
    test_decision_is_final();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("BOOT-POLICY SIMULATION FAILED\n");
        return 1;
    }
    printf("BOOT-POLICY SIMULATION PASSED\n");
    return 0;
}
