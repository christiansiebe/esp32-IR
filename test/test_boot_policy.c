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

/* --- Many failure cases must trigger recovery (rollback) ------------------ */

/* A dashboard that is up only inside [on_ms, off_ms) (off_ms == 0: stays up
 * forever, on_ms == 0: never up). Models a subsystem that comes up and then
 * dies, which the earlier single-shot simulate() could not express. */
static uint32_t g_dash_on_ms;
static uint32_t g_dash_off_ms;

static bool dash_window(uint32_t t)
{
    if (g_dash_on_ms == 0 || t < g_dash_on_ms) {
        return false;
    }
    if (g_dash_off_ms != 0 && t >= g_dash_off_ms) {
        return false;
    }
    return true;
}

static boot_action_t simulate_window(uint32_t on_ms, uint32_t off_ms, uint32_t *at)
{
    g_dash_on_ms = on_ms;
    g_dash_off_ms = off_ms;
    boot_policy_t policy;
    boot_policy_defaults(&policy);
    boot_state_t state;
    boot_state_reset(&state);
    for (uint32_t t = 0; t <= 300000u; t += 1000u) {
        boot_inputs_t in = {.is_pending_verify = true, .dashboard_ok = dash_window(t)};
        boot_action_t a = boot_evaluate(&policy, &state, t, &in);
        if (a != BOOT_TAKE_NONE) {
            if (at != NULL) {
                *at = t;
            }
            return a;
        }
    }
    return BOOT_TAKE_NONE;
}

typedef struct {
    const char *name;
    uint32_t on_ms;
    uint32_t off_ms;
    boot_action_t expect;
} boot_scenario_t;

static void test_failure_cases_roll_back(void)
{
    /* Every pattern where the dashboard is not usable at the 30 s grace
     * deadline (or dies before the 45 s stability window) must roll back. The
     * two healthy patterns are included to prove recovery is not over-eager. */
    static const boot_scenario_t scenarios[] = {
        {"never comes up",              0,      0,      BOOT_TAKE_ROLLBACK},
        {"up 2s, dies at 3s",           2000,   3000,   BOOT_TAKE_ROLLBACK},
        {"up 2s, dies at 20s",          2000,   20000,  BOOT_TAKE_ROLLBACK},
        {"up 2s, dies at 29s",          2000,   29000,  BOOT_TAKE_ROLLBACK},
        {"up 2s, dies at 40s",          2000,   40000,  BOOT_TAKE_ROLLBACK},
        {"up 2s, dies at 44s",          2000,   44000,  BOOT_TAKE_ROLLBACK},
        {"comes up only at 45s",        45000,  0,      BOOT_TAKE_ROLLBACK},
        {"comes up only at 60s",        60000,  0,      BOOT_TAKE_ROLLBACK},
        {"comes up only at 31s",        31000,  0,      BOOT_TAKE_ROLLBACK},
        {"up 2s and stays",             2000,   0,      BOOT_TAKE_CONFIRM},
        {"up exactly at grace (30s)",   30000,  0,      BOOT_TAKE_CONFIRM},
    };
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        uint32_t at = 0;
        boot_action_t a = simulate_window(scenarios[i].on_ms, scenarios[i].off_ms, &at);
        printf("  scenario %-26s -> %-24s at %u.%03us\n", scenarios[i].name,
               action_name(a), at / 1000u, at % 1000u);
        CHECK(a == scenarios[i].expect, scenarios[i].name);
    }
}

static void test_flapping_dashboard_rolls_back(void)
{
    /* The dashboard comes up three times for a few seconds each and is gone by
     * the grace deadline: the image is not usable and must roll back. */
    static const uint32_t up_from[] = {2000, 10000, 20000};
    static const uint32_t up_to[] = {5000, 12000, 21000};
    boot_policy_t policy;
    boot_policy_defaults(&policy);
    boot_state_t state;
    boot_state_reset(&state);
    boot_action_t action = BOOT_TAKE_NONE;
    for (uint32_t t = 0; t <= 300000u; t += 1000u) {
        bool up = false;
        for (size_t i = 0; i < 3; ++i) {
            if (t >= up_from[i] && t < up_to[i]) {
                up = true;
            }
        }
        boot_inputs_t in = {.is_pending_verify = true, .dashboard_ok = up};
        boot_action_t a = boot_evaluate(&policy, &state, t, &in);
        if (a != BOOT_TAKE_NONE) {
            action = a;
            break;
        }
    }
    CHECK(action == BOOT_TAKE_ROLLBACK, "flapping dashboard rolls back");
}

static void test_rollback_never_before_grace(void)
{
    /* With the dashboard never up, no action is taken before the 30 s grace;
     * a premature rollback would reboot a device that is still starting. */
    boot_policy_t policy;
    boot_policy_defaults(&policy);
    boot_state_t state;
    boot_state_reset(&state);
    for (uint32_t t = 0; t < 30000u; t += 1000u) {
        boot_inputs_t in = {.is_pending_verify = true, .dashboard_ok = false};
        CHECK(boot_evaluate(&policy, &state, t, &in) == BOOT_TAKE_NONE,
              "no rollback before the grace window");
    }
    boot_inputs_t in = {.is_pending_verify = true, .dashboard_ok = false};
    CHECK(boot_evaluate(&policy, &state, 30000u, &in) == BOOT_TAKE_ROLLBACK,
          "rollback fires exactly at the grace window");
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
    test_failure_cases_roll_back();
    test_flapping_dashboard_rolls_back();
    test_rollback_never_before_grace();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("BOOT-POLICY SIMULATION FAILED\n");
        return 1;
    }
    printf("BOOT-POLICY SIMULATION PASSED\n");
    return 0;
}
