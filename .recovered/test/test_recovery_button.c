/* Host simulator for the recovery-button gesture. No ESP-IDF.
 *
 * Replays button press patterns tick by tick and asserts the decision. */
#include <stdio.h>

#include "recovery_button.h"

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

/* Feed a press pattern: `pressed_until_ms` = button held from t=0 to that time
 * (0 = never pressed). Returns the first action, and the uptime it happened. */
static recovery_action_t simulate(uint32_t pressed_until_ms,
                                  uint32_t hold_ms, uint32_t *at_ms)
{
    recovery_state_t state;
    recovery_state_reset(&state);
    for (uint32_t t = 0; t <= 60000u; t += 25u) {
        bool pressed = (pressed_until_ms != 0 && t < pressed_until_ms);
        recovery_action_t a =
            recovery_evaluate(&state, t, pressed, hold_ms);
        if (a != RECOVERY_IDLE) {
            if (at_ms) {
                *at_ms = t;
            }
            return a;
        }
    }
    return RECOVERY_IDLE;
}

static void test_hold_three_seconds_forces(void)
{
    uint32_t at = 0;
    recovery_action_t a = simulate(10000, 3000, &at);
    CHECK(a == RECOVERY_FORCE, "3 s hold forces rollback");
    CHECK(at >= 3000u && at < 3100u, "fires shortly after the hold window");
    printf("  trace hold 10s (need 3s)  -> at %u.%03us: FORCE\n", at / 1000,
           at % 1000);
}

static void test_short_press_ignored(void)
{
    CHECK(simulate(1500, 3000, NULL) == RECOVERY_IDLE,
          "a 1.5 s press does not force a rollback");
    printf("  trace tap 1.5s (need 3s)  -> idle (no rollback)\n");
}

static void test_release_restarts_timer(void)
{
    /* Pressed 2 s, released, pressed again 2 s: neither hold reaches 3 s. */
    recovery_state_t state;
    recovery_state_reset(&state);
    recovery_action_t a = RECOVERY_IDLE;
    for (uint32_t t = 0; t <= 6000u; t += 25u) {
        bool pressed = (t < 2000u) || (t >= 3000u && t < 5000u);
        a = recovery_evaluate(&state, t, pressed, 3000u);
        if (a == RECOVERY_FORCE) {
            break;
        }
    }
    CHECK(a == RECOVERY_IDLE, "two short holds do not add up");
    printf("  trace 2s+2s with gap       -> idle (no rollback)\n");
}

static void test_custom_hold_window(void)
{
    uint32_t at = 0;
    CHECK(simulate(6000, 1000, &at) == RECOVERY_FORCE, "1 s window honoured");
    CHECK(at >= 1000u && at < 1100u, "fires near the 1 s window");
}

int main(void)
{
    printf("recovery-button simulator\n");
    test_hold_three_seconds_forces();
    test_short_press_ignored();
    test_release_restarts_timer();
    test_custom_hold_window();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("RECOVERY-BUTTON SIMULATION FAILED\n");
        return 1;
    }
    printf("RECOVERY-BUTTON SIMULATION PASSED\n");
    return 0;
}
