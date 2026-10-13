/* Host tests for the adaptive UART light-sleep window (main/uart_sleep_window.h).
 *
 * The controller decides how long the sender may park the CPU between SML
 * telegrams. The arithmetic must never let the window exceed the shortest quiet
 * interval it has observed, and must back off when a sleep was too long. */
#include <stdio.h>

#include "uart_sleep_window.h"

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

#define GAP_MS 50u

static void test_starts_at_initial_and_does_not_sleep_idle(void)
{
    uart_sleep_ctrl_t s;
    uart_sleep_init(&s, uart_sleep_default_cfg());
    CHECK(s.window_ms == 400, "starts at 400 ms");
    CHECK(!uart_sleep_should_request(&s), "does not sleep before any telegram");
}

static void test_min_gap_caps_the_window(void)
{
    uart_sleep_ctrl_t s;
    uart_sleep_init(&s, uart_sleep_default_cfg());

    /* Bytes flowing, then a 730 ms quiet gap observed between two bursts. */
    uint64_t t = 1000000;
    uart_sleep_note_byte(&s, t, 0, GAP_MS);          /* first byte, no prior */
    CHECK(s.have_burst, "burst marked while bytes flow");
    t += 1000; /* +1 ms: still within a burst */
    uart_sleep_note_byte(&s, t, t - 1000, GAP_MS);
    CHECK(s.have_burst, "still in burst for a 1 ms gap");

    /* Next burst starts 730 ms after the previous byte: quiet interval. */
    uart_sleep_note_byte(&s, t + 730000, t, GAP_MS);
    CHECK(s.last_min_gap_ms == 730, "records the 730 ms quiet gap");
    CHECK(s.window_ms == 400, "window capped below min_gap (730*3/4=547, stays 400)");

    /* A shorter quiet gap must tighten the window to 3/4 of it. */
    uint64_t last = t + 730000;
    uart_sleep_note_byte(&s, last + 200000, last, GAP_MS);
    CHECK(s.last_min_gap_ms == 200, "records the shorter 200 ms gap");
    CHECK(s.window_ms == 150, "window tightened to 3/4 of 200 ms");
}

static void test_clean_sleep_grows_but_respects_the_cap(void)
{
    uart_sleep_ctrl_t s;
    uart_sleep_init(&s, uart_sleep_default_cfg());
    s.last_min_gap_ms = 700;
    uart_sleep_apply_cap(&s);

    uint32_t before = s.window_ms;
    uart_sleep_note_clean(&s);
    CHECK(s.window_ms == before + 25, "grows by grow_ms after a clean sleep");

    /* Growth must never exceed the min_gap cap. */
    for (int i = 0; i < 100; ++i) {
        uart_sleep_note_clean(&s);
    }
    CHECK(s.window_ms == 525, "growth stops at the min_gap cap (700*3/4)");
}

static void test_short_sleep_halves_the_window(void)
{
    uart_sleep_ctrl_t s;
    uart_sleep_init(&s, uart_sleep_default_cfg());
    s.last_min_gap_ms = 800;
    uart_sleep_apply_cap(&s);
    CHECK(s.window_ms == 400, "400 ms initial against an 800 ms min gap");

    uart_sleep_note_short_sleep(&s);
    CHECK(s.window_ms == 200, "short sleep halves the window");
    uart_sleep_note_short_sleep(&s);
    CHECK(s.window_ms == 100, "halving again reaches the floor");
    uart_sleep_note_short_sleep(&s);
    CHECK(s.window_ms == 100, "never goes below min_ms");
}

static void test_burst_blocks_sleep(void)
{
    uart_sleep_ctrl_t s;
    uart_sleep_init(&s, uart_sleep_default_cfg());
    s.last_min_gap_ms = 500;
    uart_sleep_apply_cap(&s);
    CHECK(uart_sleep_should_request(&s), "sleeps in the quiet gap");

    uart_sleep_note_byte(&s, 2000000, 0, GAP_MS);
    CHECK(!uart_sleep_should_request(&s), "no sleep while a burst is active");
    uart_sleep_burst_idle(&s);
    CHECK(uart_sleep_should_request(&s), "sleeps again once the burst is idle");
}

int main(void)
{
    test_starts_at_initial_and_does_not_sleep_idle();
    test_min_gap_caps_the_window();
    test_clean_sleep_grows_but_respects_the_cap();
    test_short_sleep_halves_the_window();
    test_burst_blocks_sleep();

    printf("uart sleep window: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
