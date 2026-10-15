/* What happens when the adaptive sender light sleep "misbehaves".
 *
 * test_uart_sleep_sim.c proves the controller settles and never clips on a
 * clean stream. This test answers the failure question instead: when the sleep
 * window is wrong, where does the loss end up, and can anything recover it?
 *
 * It drives the real uart_sleep_window.h controller against a synthetic SML
 * read head and models the exact production consequence: light sleep stops the
 * APB clock with no UART wake source, so a telegram that starts inside a sleep
 * window loses its leading bytes -> the checksum fails -> that telegram carries
 * no sample. A minute is *measured* only if at least one of its telegrams
 * decoded; a minute whose every telegram was clipped is a REAL hole (the sender
 * never decoded it), which no backfill or boot marker can repair. That is the
 * difference the dashboard must show: real loss, not a recovered radio gap.
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
    uint32_t bursts;         /* telegrams the read head emitted */
    uint32_t clipped;        /* telegrams that started inside a sleep window */
    uint32_t sleeps;
    uint32_t window_ms;
    uint32_t min_gap_ms;
    /* Per-minute outcome: a minute is 60 s of read-head time. */
    uint32_t minutes;        /* whole minutes simulated */
    uint32_t minutes_lost;   /* every telegram in the minute was clipped */
} loss_result_t;

static int32_t next_jitter(uint32_t *state, int32_t jitter)
{
    *state = *state * 1664525u + 1013904223u;
    if (jitter <= 0) {
        return 0;
    }
    int32_t span = 2 * jitter + 1;
    return (int32_t)((*state >> 16) % (uint32_t)span) - jitter;
}

/* Run the stream for `cycles` telegrams. The spacing is `period_a` until
 * `switch_after` telegrams, then `period_b`, so a sudden speed-up of the read
 * head can be forced (the only way to make the learned window too long). Jitter
 * is applied to both. Resolve every sleep against the next burst exactly like
 * the IDF glue does, and bucket the outcome into minutes. `hold_active` mirrors
 * uart_sleep_hold(): while true the controller must not enter a sleep window. */
static loss_result_t simulate_losses(uint32_t period_a, uint32_t switch_after,
                                     uint32_t period_b, int32_t jitter_ms,
                                     uint32_t cycles, uint32_t max_window_ms,
                                     bool hold_active, uint32_t *rng)
{
    uart_sleep_ctrl_t ctrl;
    uart_sleep_init(&ctrl, uart_sleep_default_cfg());
    ctrl.cfg.max_ms = max_window_ms;

    loss_result_t r = {0};
    uint32_t state = *rng;

    uint32_t next_burst = 1000;
    uint32_t last_byte_ms = 0;
    bool have_last = false;
    bool sleeping = false;
    uint32_t wake_at = 0;

    /* Per-minute accounting: bucket index = burst_time / 60000. */
    uint32_t cur_minute = 0;
    uint32_t good_in_minute = 0;
    bool have_minute = false;

    for (uint32_t i = 0; i < cycles; ++i) {
        bool clipped = false;
        if (sleeping) {
            if (wake_at <= next_burst) {
                uart_sleep_note_clean(&ctrl);
            } else {
                clipped = true;
                uart_sleep_note_short_sleep(&ctrl);
            }
            sleeping = false;
        }

        /* Bucket into minutes. */
        uint32_t minute = next_burst / 60000u;
        if (!have_minute) {
            cur_minute = minute;
            have_minute = true;
        } else if (minute != cur_minute) {
            ++r.minutes;
            if (good_in_minute == 0) {
                ++r.minutes_lost;
            }
            cur_minute = minute;
            good_in_minute = 0;
        }
        if (!clipped) {
            ++good_in_minute;
        }
        ++r.bursts;
        if (clipped) {
            ++r.clipped;
        }

        if (have_last) {
            uart_sleep_note_byte(&ctrl, (uint64_t)next_burst * 1000ULL,
                                 (uint64_t)last_byte_ms * 1000ULL, GAP_THRESHOLD_MS,
                                 true);
        }
        uart_sleep_note_byte(&ctrl, (uint64_t)(next_burst + 1u) * 1000ULL,
                             (uint64_t)next_burst * 1000ULL, GAP_THRESHOLD_MS, true);
        last_byte_ms = next_burst + BURST_MS;
        have_last = true;

        uint32_t idle_at = last_byte_ms + GAP_THRESHOLD_MS;
        uart_sleep_burst_idle(&ctrl);
        /* The IDF glue refuses to sleep while a hold is active (uart_sleep_hold)
         * and checks the pure predicate before releasing the PM lock. */
        int64_t now_us = (int64_t)idle_at * 1000;
        bool held = hold_active && uart_sleep_hold_active(now_us + 1000000, now_us);
        if (uart_sleep_should_request(&ctrl) && !held) {
            sleeping = true;
            wake_at = idle_at + ctrl.window_ms;
            r.sleeps++;
        }

        int32_t j = next_jitter(&state, jitter_ms);
        uint32_t period = (i < switch_after) ? period_a : period_b;
        int64_t nb = (int64_t)next_burst + (int64_t)period + j;
        int64_t floor_ms = (int64_t)last_byte_ms + GAP_THRESHOLD_MS + 10;
        if (nb < floor_ms) {
            nb = floor_ms;
        }
        next_burst = (uint32_t)nb;
    }
    /* Close the last open minute. */
    if (have_minute) {
        ++r.minutes;
        if (good_in_minute == 0) {
            ++r.minutes_lost;
        }
    }

    r.window_ms = ctrl.window_ms;
    r.min_gap_ms = ctrl.last_min_gap_ms;
    *rng = state;
    return r;
}

int main(void)
{
    uint32_t rng;

    /* 1. Steady read head: the controller adapts, clips nothing, and therefore
     *    every minute is measured - no real loss. */
    rng = 1;
    loss_result_t a = simulate_losses(1000, 400, 1000, 120, 400, 1500, false, &rng);
    printf("steady 1000ms : bursts=%u clipped=%u minutes=%u lost=%u window=%u\n",
           a.bursts, a.clipped, a.minutes, a.minutes_lost, a.window_ms);
    CHECK(a.clipped == 0, "steady stream: nothing clipped");
    CHECK(a.minutes_lost == 0, "steady stream: no minute without a sample");

    /* 2. A single, permanent speed-up (head now faster than the learned window):
     *    the controller clips a few telegrams while it backs off, but each
     *    affected minute still holds other good telegrams, so NO minute is lost.
     *    This is the value of the adaptive window: transient misbehaviour is
     *    absorbed and stays recoverable. */
    rng = 2;
    loss_result_t b = simulate_losses(2500, 100, 700, 0, 400, 1500, false, &rng);
    printf("switch 2500->700: bursts=%u clipped=%u minutes=%u lost=%u window=%u\n",
           b.bursts, b.clipped, b.minutes, b.minutes_lost, b.window_ms);
    CHECK(b.clipped > 0, "a sudden speed-up does clip at least once");
    CHECK(b.minutes_lost == 0, "transient clipping never empties a whole minute");

    /* 3. Persistent misbehaviour: the read head fires far faster than the
     *    controller's floor allows (period below min window). Telegrams keep
     *    starting inside the sleep window, so whole minutes lose every sample.
     *    That is a REAL hole the sender never decoded - backfill cannot repair
     *    it and the boot marker must not hide it. The test only asserts it is
     *    detected (minutes_lost > 0), so the dashboard reports real loss. */
    rng = 3;
    loss_result_t c = simulate_losses(150, 2000, 150, 0, 2000, 1500, false, &rng);
    printf("jam    150ms  : bursts=%u clipped=%u minutes=%u lost=%u window=%u\n",
           c.bursts, c.clipped, c.minutes, c.minutes_lost, c.window_ms);
    CHECK(c.clipped > 0, "a jammed head is clipped");
    CHECK(c.minutes_lost > 0, "persistent clipping is reported as real loss");

    /* 4. Hold gate: while an ESP-NOW request (history pull / OTA) is in flight,
     *    uart_sleep_hold() keeps the pure predicate true, so the glue never
     *    releases the PM lock. No sleep happens, the exchange is not starved. */
    rng = 4;
    loss_result_t d = simulate_losses(1000, 400, 1000, 120, 400, 1500, true, &rng);
    printf("held   1000ms : bursts=%u clipped=%u minutes=%u lost=%u sleeps=%u\n",
           d.bursts, d.clipped, d.minutes, d.minutes_lost, d.sleeps);
    CHECK(d.sleeps == 0, "an active hold suppresses every sleep window");
    CHECK(d.minutes_lost == 0, "held stream loses no minute");

    /* 5. The pure hold predicate itself: set -> active, expired -> inactive. */
    CHECK(uart_sleep_hold_active(1000000, 999999), "hold active before its deadline");
    CHECK(!uart_sleep_hold_active(1000000, 1000000), "hold inactive at the deadline");
    CHECK(!uart_sleep_hold_active(0, 12345), "no hold set is never active");

    printf("sleep misbehavior: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
