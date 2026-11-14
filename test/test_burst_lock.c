/* Host tests for the burst-locked receive window (main/burst_lock.c).
 *
 * The module is pure C, so burst detection, period learning, the wake
 * prediction and the re-learn fallback can be checked without a radio. */
#include <stdio.h>

#include "burst_lock.h"

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

/* Feed a burst: `count` frames `spacing` ms apart starting at `start`. */
static void feed_burst(burst_lock_t *bl, uint32_t start, int count,
                       uint32_t spacing)
{
    for (int i = 0; i < count; ++i) {
        (void)burst_lock_note_frame(bl, start + (uint32_t)i * spacing);
    }
}

static void test_init_defaults(void)
{
    burst_lock_t bl;
    burst_lock_init(&bl, 0, 0, 0, 0);
    CHECK(bl.gap_ms == 1000u, "default gap");
    CHECK(bl.window_ms == 400u, "default window");
    CHECK(bl.max_misses == 3u, "default max misses");
    CHECK(!burst_lock_locked(&bl), "not locked before any burst");
    CHECK(burst_lock_wait_ms(&bl, 0) == 0, "wait is 0 while unlocked");

    burst_lock_init(&bl, 800, 300, 50, 5);
    CHECK(bl.gap_ms == 800u && bl.window_ms == 300u, "explicit timings kept");
    CHECK(bl.lead_ms == 50u, "explicit lead kept");
    CHECK(bl.max_misses == 5u, "explicit max misses kept");

    burst_lock_init(&bl, 0, 200, 500, 0);
    CHECK(bl.lead_ms < bl.window_ms, "lead capped below the window");
}

static void test_burst_detection(void)
{
    burst_lock_t bl;
    burst_lock_init(&bl, 1000, 400, 50, 3);
    CHECK(burst_lock_note_frame(&bl, 5000), "first frame is a burst");
    CHECK(!burst_lock_note_frame(&bl, 5001), "1 ms later: same burst");
    CHECK(!burst_lock_note_frame(&bl, 5003), "3 ms later: same burst");
    /* The gap is measured from the last frame, so the next burst needs a gap of
     * at least 1000 ms from 5003. */
    CHECK(!burst_lock_note_frame(&bl, 6000), "997 ms after the last frame: same burst");
    CHECK(burst_lock_note_frame(&bl, 7005), ">1000 ms after the last frame: new burst");
    CHECK(bl.bursts == 2u, "two bursts counted");
}

static void test_period_learning_and_wait(void)
{
    burst_lock_t bl;
    burst_lock_init(&bl, 1000, 400, 50, 3);
    feed_burst(&bl, 10000, 3, 2);
    CHECK(!burst_lock_locked(&bl), "one burst is not enough to lock");
    feed_burst(&bl, 15000, 3, 2);
    CHECK(burst_lock_locked(&bl), "locked after two bursts");
    CHECK(bl.period_ms == 5000u, "period learned as 5000 ms");

    /* The next burst is due at 20000; with a 50 ms lead the window opens at
     * 19950. From 19000 that is 950 ms away. */
    CHECK(burst_lock_wait_ms(&bl, 19000) == 950, "wait until the window opens");
    CHECK(burst_lock_wait_ms(&bl, 19950) == 0, "window opens now");
    CHECK(burst_lock_wait_ms(&bl, 20050) == 0, "overdue window opens now");
}

static void test_window_served_and_miss(void)
{
    burst_lock_t bl;
    burst_lock_init(&bl, 1000, 400, 50, 3);
    feed_burst(&bl, 10000, 2, 2);
    feed_burst(&bl, 15000, 2, 2);
    CHECK(burst_lock_locked(&bl), "locked");

    /* A window that catches the burst counts as a hit and clears the misses. */
    burst_lock_window_open(&bl);
    feed_burst(&bl, 20000, 2, 2);
    burst_lock_window_close(&bl);
    CHECK(bl.window_hits == 1u, "window hit counted");
    CHECK(bl.misses == 0u, "misses cleared by a hit");
    CHECK(burst_lock_locked(&bl), "still locked after a hit");

    /* Three empty windows in a row drop the lock so the caller re-learns. */
    for (int i = 0; i < 2; ++i) {
        burst_lock_window_open(&bl);
        burst_lock_window_close(&bl);
    }
    CHECK(burst_lock_locked(&bl), "still locked after two misses");
    burst_lock_window_open(&bl);
    burst_lock_window_close(&bl);
    CHECK(!burst_lock_locked(&bl), "unlocked after three misses");
    CHECK(bl.unlocks == 1u, "unlock counted");
    CHECK(bl.window_misses == 3u, "misses counted");
    CHECK(!bl.have_burst, "burst history cleared for re-learning");
}

static void test_implausible_period_ignored(void)
{
    burst_lock_t bl;
    burst_lock_init(&bl, 1000, 400, 50, 3);
    feed_burst(&bl, 1000, 2, 2);
    /* A 500 ms gap is below the plausible floor: not a period. */
    feed_burst(&bl, 1500, 2, 2);
    CHECK(!burst_lock_locked(&bl), "too-short period rejected");
    /* 30 s is above the ceiling: also rejected. */
    feed_burst(&bl, 31500, 2, 2);
    CHECK(!burst_lock_locked(&bl), "too-long period rejected");
}

static void test_rollover(void)
{
    burst_lock_t bl;
    burst_lock_init(&bl, 1000, 400, 50, 3);
    uint32_t near_wrap = 0xFFFFFF00u;
    feed_burst(&bl, near_wrap, 2, 2);
    /* The next burst lands just after the 32-bit ms rollover. */
    feed_burst(&bl, near_wrap + 5000u, 2, 2);
    CHECK(burst_lock_locked(&bl), "locked across the ms rollover");
    CHECK(bl.period_ms == 5000u, "period correct across the rollover");
}

static void test_window_extension(void)
{
    burst_lock_t bl;
    burst_lock_init(&bl, 1000, 400, 50, 3);
    feed_burst(&bl, 10000, 2, 2);
    feed_burst(&bl, 15000, 2, 2);
    CHECK(burst_lock_locked(&bl), "locked");

    /* A burst that now spans the sender's ACK round trips can run past one
     * window: keep it open while frames keep landing, up to the hard cap. */
    burst_lock_window_open(&bl);
    feed_burst(&bl, 20000, 2, 2); /* last frame at 20002 */
    CHECK(burst_lock_window_should_extend(&bl, 19950, 20050, 4000),
          "extend while a frame just landed");
    CHECK(!burst_lock_window_should_extend(&bl, 19950, 20200, 4000),
          "quiet gap ends the extension");
    CHECK(!burst_lock_window_should_extend(&bl, 19950, 19950 + 4000, 4000),
          "hard cap ends the extension");
    burst_lock_window_close(&bl);

    /* Closed window never extends. */
    CHECK(!burst_lock_window_should_extend(&bl, 20000, 20010, 4000),
          "closed window does not extend");
}

static void test_idle_poll_cadence(void)
{
    /* Fast whenever a burst could still be missed: locked, an exchange in
     * flight, or still learning. Slow only once just a re-learn trigger is
     * awaited, so an idle receiver stops waking 20x/s for nothing. */
    CHECK(burst_lock_idle_poll_ms(true, false, false, 50, 500) == 50u,
          "locked keeps the fast poll");
    CHECK(burst_lock_idle_poll_ms(false, true, false, 50, 500) == 50u,
          "a held exchange keeps the fast poll");
    CHECK(burst_lock_idle_poll_ms(false, false, true, 50, 500) == 50u,
          "still learning keeps the fast poll");
    CHECK(burst_lock_idle_poll_ms(false, false, false, 50, 500) == 500u,
          "settled idle uses the slow poll");
}

static void test_forget(void)
{
    burst_lock_t bl;
    burst_lock_init(&bl, 1000, 400, 50, 3);
    feed_burst(&bl, 10000, 2, 2);
    feed_burst(&bl, 15000, 2, 2);
    CHECK(burst_lock_locked(&bl), "locked before forget");
    uint32_t bursts = bl.bursts;

    burst_lock_forget(&bl);
    CHECK(!burst_lock_locked(&bl), "forget drops the lock");
    CHECK(bl.period_ms == 0u, "forget clears the period");
    CHECK(!bl.in_window && !bl.window_served, "forget clears the window state");
    CHECK(bl.bursts == bursts, "forget keeps the counters");
}

int main(void)
{
    test_init_defaults();
    test_burst_detection();
    test_period_learning_and_wait();
    test_window_served_and_miss();
    test_implausible_period_ignored();
    test_rollover();
    test_window_extension();
    test_idle_poll_cadence();
    test_forget();

    printf("burst lock: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
