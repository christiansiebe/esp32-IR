/* Timing simulation for the adaptive UART light-sleep controller.
 *
 * test_uart_sleep_window.c unit-tests the arithmetic; this drives the controller
 * against a synthetic SML read head and models the ESP32 reality: light sleep
 * stops the APB clock and there is no UART wake-up source, so a telegram that
 * arrives while the CPU sleeps loses its leading bytes - the exact failure the
 * controller exists to avoid. The sim mirrors the lock/sleep glue in
 * main/uart_sleep_window.c: the CPU wakes only when the adaptive window expires,
 * is awake for the whole burst, then sleeps again in the quiet gap.
 *
 * It answers three questions:
 *   1. Does the window settle, or oscillate forever?
 *   2. Does a longer telegram period let it sleep longer (does it seek the
 *      optimum) without clipping?
 *   3. Do out-of-pattern early bursts cost only a bounded, one-time number of
 *      clippings before the window adapts?
 */
#include <stdint.h>
#include <stdio.h>

#include "uart_sleep_window.h"

/* 9600 baud, 8N1: 10 bits per byte. One telegram is a ~60 ms burst of bytes. */
#define BYTE_US 1042u
#define TELEGRAM_BYTES 58u
#define TELEGRAM_MS ((TELEGRAM_BYTES * BYTE_US + 999u) / 1000u) /* 61 ms */
#define BURST_MS TELEGRAM_MS
#define GAP_THRESHOLD_MS 50u /* mirrors UART_SLEEP_GAP_THRESHOLD_MS */

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

typedef struct {
    uint32_t window_ms;
    uint32_t min_gap_ms;
    uint32_t sleeps;
    uint32_t sleep_ms;
    uint32_t overruns; /* sleeps a telegram arrived during */
    uint32_t clipped;  /* telegram bursts that lost leading bytes */
} sim_result_t;

static int32_t next_jitter(uint32_t *state, int32_t jitter)
{
    *state = *state * 1664525u + 1013904223u;
    if (jitter <= 0) {
        return 0;
    }
    int32_t span = 2 * jitter + 1;
    return (int32_t)((*state >> 16) % (uint32_t)span) - jitter;
}

/* Burst-start-to-burst-start period, in ms. max_window_ms overrides the
 * production 1500 ms cap so the sim can show the controller seeking an optimum.
 * The period switches from period_a to period_b after switch_after cycles, so a
 * sudden faster read head can be forced (the only way to make the window too
 * long and exercise the back-off). */
static sim_result_t simulate(uint32_t period_a, uint32_t switch_after,
                             uint32_t period_b, int32_t jitter_ms, uint32_t cycles,
                             uint32_t max_window_ms, uint32_t *rng)
{
    uart_sleep_ctrl_t ctrl;
    uart_sleep_init(&ctrl, uart_sleep_default_cfg());
    ctrl.cfg.max_ms = max_window_ms;

    sim_result_t r = {0};
    uint32_t state = *rng;

    uint32_t next_burst = 1000; /* let the controller see one quiet gap first */
    uint32_t last_byte_ms = 0;
    bool have_last = false;
    bool sleeping = false;
    uint32_t wake_at = 0;

    for (uint32_t i = 0; i < cycles; ++i) {
        /* Resolve a sleep against the imminent burst. */
        if (sleeping) {
            if (wake_at <= next_burst) {
                uart_sleep_note_clean(&ctrl); /* woke in time: clean window */
            } else {
                r.overruns++;
                r.clipped++;                  /* burst head lost to the APB stop */
                uart_sleep_note_short_sleep(&ctrl);
            }
            sleeping = false;
        }

        /* The burst arrives: one gap byte, then the telegram bytes. */
        if (have_last) {
            uart_sleep_note_byte(&ctrl, (uint64_t)next_burst * 1000ULL,
                                 (uint64_t)last_byte_ms * 1000ULL, GAP_THRESHOLD_MS,
                                 true);
        }
        /* Second byte 1 ms later: shows a burst is open (gap < threshold). */
        uart_sleep_note_byte(&ctrl, (uint64_t)(next_burst + 1u) * 1000ULL,
                             (uint64_t)next_burst * 1000ULL, GAP_THRESHOLD_MS, true);
        last_byte_ms = next_burst + BURST_MS;
        have_last = true;

        /* The UART read times out 50 ms after the last byte: sleep for the
         * adaptive window before the next telegram. */
        uint32_t idle_at = last_byte_ms + GAP_THRESHOLD_MS;
        uart_sleep_burst_idle(&ctrl);
        if (uart_sleep_should_request(&ctrl)) {
            sleeping = true;
            wake_at = idle_at + ctrl.window_ms;
            r.sleeps++;
            r.sleep_ms += ctrl.window_ms;
        }

        /* Schedule the next burst. */
        uint32_t period = (i < switch_after) ? period_a : period_b;
        int32_t j = next_jitter(&state, jitter_ms);
        int64_t nb = (int64_t)next_burst + (int64_t)period + j;
        int64_t floor_ms = (int64_t)last_byte_ms + GAP_THRESHOLD_MS + 10;
        if (nb < floor_ms) {
            nb = floor_ms;
        }
        next_burst = (uint32_t)nb;
    }

    r.window_ms = ctrl.window_ms;
    r.min_gap_ms = ctrl.last_min_gap_ms;
    *rng = state;
    return r;
}

int main(void)
{
    uint32_t rng;

    /* 1. Settling on a steady ~1 s read head. */
    rng = 1;
    sim_result_t a = simulate(1000, 200, 1000, 120, 80, 1500, &rng);
    printf("steady 1000ms: window=%4u min_gap=%4u sleeps=%3u overruns=%u clipped=%u\n",
           a.window_ms, a.min_gap_ms, a.sleeps, a.overruns, a.clipped);
    CHECK(a.sleeps > 30, "steady stream still sleeps often");
    CHECK(a.window_ms > 400, "window grew past the 400 ms start");
    CHECK(a.window_ms <= (a.min_gap_ms * 3u) / 4u, "window never exceeds 3/4 of min gap");
    CHECK(a.overruns == 0, "no overrun on a steady stream");
    CHECK(a.clipped == 0, "no telegram clipped on a steady stream");

    /* 2. Optimum: a slower read head (2.5 s) must allow a longer sleep. */
    rng = 2;
    sim_result_t b = simulate(2500, 200, 2500, 120, 50, 5000, &rng);
    printf("slow   2500ms: window=%4u min_gap=%4u sleeps=%3u overruns=%u clipped=%u\n",
           b.window_ms, b.min_gap_ms, b.sleeps, b.overruns, b.clipped);
    CHECK(b.window_ms > a.window_ms, "longer telegram period -> longer sleep");
    CHECK(b.clipped == 0, "no clipping when the read head is slower");

    /* 3. Faster read head (800 ms) must tighten the window, still no clipping. */
    rng = 3;
    sim_result_t c = simulate(800, 200, 800, 60, 80, 1500, &rng);
    printf("fast    800ms: window=%4u min_gap=%4u sleeps=%3u overruns=%u clipped=%u\n",
           c.window_ms, c.min_gap_ms, c.sleeps, c.overruns, c.clipped);
    CHECK(c.window_ms < a.window_ms, "shorter telegram period -> shorter sleep");
    CHECK(c.clipped == 0, "no clipping on a faster stream");

    /* 4. The read head suddenly speeds up: a long-trained window is too big, so
     *    the controller must clip at most a few telegrams and then adapt down. */
    rng = 4;
    sim_result_t d = simulate(2500, 40, 800, 0, 80, 1500, &rng);
    printf("switch 2500->800: window=%4u min_gap=%4u sleeps=%3u overruns=%u clipped=%u\n",
           d.window_ms, d.min_gap_ms, d.sleeps, d.overruns, d.clipped);
    CHECK(d.clipped >= 1, "a sudden speed-up is caught (clips at least once)");
    CHECK(d.clipped <= 6, "the controller adapts within a few telegrams");
    CHECK(d.window_ms < 800, "window backed off after the speed-up");

    /* 5. Out-of-pattern jitter: clipping stays bounded, never sustained. */
    rng = 5;
    sim_result_t e = simulate(1200, 200, 1200, 300, 80, 1500, &rng);
    printf("jittery 1200ms: window=%4u min_gap=%4u sleeps=%3u overruns=%u clipped=%u\n",
           e.window_ms, e.min_gap_ms, e.sleeps, e.overruns, e.clipped);
    CHECK(e.window_ms <= (e.min_gap_ms * 3u) / 4u, "jitter still capped by the min gap");
    CHECK(e.clipped <= 12, "clipping bounded under jitter, not sustained");

    printf("uart sleep sim: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
