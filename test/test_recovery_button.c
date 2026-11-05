/* Host simulator for the recovery-button gesture. No ESP-IDF.
 *
 * Replays button press patterns tick by tick and asserts the decision.
 *
 * One soldered BOOT-pin button drives two actions:
 *   - released after >= RESTORE_MS (3 s) and before ROLLBACK_MS -> RESTORE
 *   - still held at ROLLBACK_MS (10 s)                          -> ROLLBACK
 */
#include <stdio.h>

#include "recovery_button.h"

#define RESTORE_MS 3000u
#define ROLLBACK_MS 10000u

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

/* Feed a press pattern: the button is held from t=0 to `pressed_until_ms`
 * (0 = never pressed). Returns the first action, and the uptime it happened. */
static recovery_action_t simulate(uint32_t pressed_until_ms,
                                  uint32_t restore_ms, uint32_t rollback_ms,
                                  uint32_t *at_ms)
{
    recovery_state_t state;
    recovery_state_reset(&state);
    for (uint32_t t = 0; t <= 60000u; t += 25u) {
        bool pressed = (pressed_until_ms != 0 && t < pressed_until_ms);
        recovery_action_t a =
            recovery_evaluate(&state, t, pressed, restore_ms, rollback_ms);
        if (a != RECOVERY_IDLE) {
            if (at_ms) {
                *at_ms = t;
            }
            return a;
        }
    }
    return RECOVERY_IDLE;
}

static void test_hold_five_seconds_restores(void)
{
    uint32_t at = 0;
    recovery_action_t a = simulate(5000, RESTORE_MS, ROLLBACK_MS, &at);
    CHECK(a == RECOVERY_RESTORE, "5 s hold restores the config");
    CHECK(at >= 5000u && at < 5100u, "restore fires when the button is released");
    printf("  trace hold 5s (need 3s)   -> at %u.%03us: RESTORE\n", at / 1000,
           at % 1000);
}

static void test_hold_ten_seconds_rolls_back(void)
{
    uint32_t at = 0;
    recovery_action_t a = simulate(30000, RESTORE_MS, ROLLBACK_MS, &at);
    CHECK(a == RECOVERY_ROLLBACK, "a long hold forces a rollback");
    CHECK(at >= 10000u && at < 10100u, "rollback fires at the 10 s window, while held");
    printf("  trace hold 30s (need 10s) -> at %u.%03us: ROLLBACK\n", at / 1000,
           at % 1000);
}

static void test_short_press_ignored(void)
{
    CHECK(simulate(1500, RESTORE_MS, ROLLBACK_MS, NULL) == RECOVERY_IDLE,
          "a 1.5 s press does nothing");
    printf("  trace tap 1.5s (need 3s)  -> idle\n");
}

static void test_boundary_below_restore(void)
{
    /* 2.975 s is below the 3 s restore window: no action. */
    CHECK(simulate(2975, RESTORE_MS, ROLLBACK_MS, NULL) == RECOVERY_IDLE,
          "just below the restore window does nothing");
    printf("  trace tap 2.975s (need 3s) -> idle\n");
}

static void test_boundary_at_restore(void)
{
    uint32_t at = 0;
    CHECK(simulate(RESTORE_MS, RESTORE_MS, ROLLBACK_MS, &at) == RECOVERY_RESTORE,
          "exactly the restore window restores");
}

static void test_restore_does_not_fire_while_held(void)
{
    /* Held past the restore window but not released: no restore, and the timer
     * keeps running towards the rollback. */
    recovery_state_t state;
    recovery_state_reset(&state);
    CHECK(recovery_evaluate(&state, 0, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_IDLE,
          "press starts the timer");
    CHECK(recovery_evaluate(&state, 3000, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_IDLE,
          "at the restore window the button is still held: no restore yet");
    CHECK(recovery_evaluate(&state, 9999, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_IDLE,
          "just before the rollback window: still nothing");
    CHECK(recovery_evaluate(&state, 10000, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_ROLLBACK,
          "at the rollback window it rolls back");
    printf("  trace hold past 3s        -> restore deferred until release\n");
}

static void test_release_restarts_timer(void)
{
    /* Pressed 2 s, released, pressed again 2 s: neither hold reaches 3 s. */
    recovery_state_t state;
    recovery_state_reset(&state);
    recovery_action_t a = RECOVERY_IDLE;
    for (uint32_t t = 0; t <= 6000u; t += 25u) {
        bool pressed = (t < 2000u) || (t >= 3000u && t < 5000u);
        a = recovery_evaluate(&state, t, pressed, RESTORE_MS, ROLLBACK_MS);
        if (a != RECOVERY_IDLE) {
            break;
        }
    }
    CHECK(a == RECOVERY_IDLE, "two short holds do not add up");
    printf("  trace 2s+2s with gap      -> idle\n");
}

static void test_custom_windows(void)
{
    uint32_t at = 0;
    CHECK(simulate(1000, 1000, 5000, &at) == RECOVERY_RESTORE, "1 s restore honoured");
    CHECK(at >= 1000u && at < 1100u, "restore fires near the 1 s window");
    CHECK(simulate(6000, 1000, 5000, &at) == RECOVERY_ROLLBACK, "5 s rollback honoured");
    CHECK(at >= 5000u && at < 5100u, "rollback fires near the 5 s window");
}

static void test_never_pressed(void)
{
    CHECK(simulate(0, RESTORE_MS, ROLLBACK_MS, NULL) == RECOVERY_IDLE,
          "an unpressed button never acts");
}

static void test_reset_clears_partial_hold(void)
{
    /* A hold in progress, then a reset: the timer must start over. */
    recovery_state_t state;
    recovery_state_reset(&state);
    CHECK(recovery_evaluate(&state, 0, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_IDLE,
          "press starts the timer");
    CHECK(recovery_evaluate(&state, 2000, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_IDLE,
          "still inside the window");
    recovery_state_reset(&state);
    CHECK(recovery_evaluate(&state, 2500, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_IDLE,
          "after reset the timer restarts at the current uptime");
    CHECK(recovery_evaluate(&state, 12499, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_IDLE,
          "no rollback until the full window from the reset");
    CHECK(recovery_evaluate(&state, 12500, true, RESTORE_MS, ROLLBACK_MS) == RECOVERY_ROLLBACK,
          "rollback fires a full window after the reset");
}

int main(void)
{
    printf("recovery-button simulator\n");
    test_hold_five_seconds_restores();
    test_hold_ten_seconds_rolls_back();
    test_short_press_ignored();
    test_boundary_below_restore();
    test_boundary_at_restore();
    test_restore_does_not_fire_while_held();
    test_release_restarts_timer();
    test_custom_windows();
    test_never_pressed();
    test_reset_clears_partial_hold();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("RECOVERY-BUTTON SIMULATION FAILED\n");
        return 1;
    }
    printf("RECOVERY-BUTTON SIMULATION PASSED\n");
    return 0;
}
