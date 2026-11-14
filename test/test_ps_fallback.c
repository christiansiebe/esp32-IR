/* Wi-Fi modem-sleep fault detector: a *sustained* storm of STA disconnects
 * (several consecutive over-threshold windows) while a PS mode is active must
 * fall back (sticky) to the proven mode, an isolated ambient burst must not, an
 * intentional reconnect must not count, and a stable association must clear the
 * count. Pure logic, no ESP-IDF. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ps_fallback.h"

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

#define WIN_MS 60000u
#define THRESH 20u
#define SUSTAIN 3u
#define GRACE_MS 30000u
#define STABLE_MS 120000u

static void init(ps_fallback_t *fb)
{
    ps_fallback_init(fb, WIN_MS, THRESH, SUSTAIN, GRACE_MS, STABLE_MS, 0);
}

static void init_sustain(ps_fallback_t *fb, uint32_t sustain)
{
    ps_fallback_init(fb, WIN_MS, THRESH, sustain, GRACE_MS, STABLE_MS, 0);
}

/* Feed `n` disconnects into the window starting at `base_ms`, 100 ms apart. */
static void storm(ps_fallback_t *fb, uint64_t base_ms, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        ps_fallback_note_disconnect(fb, base_ms + 100u * (i + 1u));
    }
}

int main(void)
{
    /* An isolated over-threshold window is only a candidate, not a fault. */
    ps_fallback_t one;
    init(&one);
    storm(&one, 0, 25); /* 25 in [0,60s) - over the threshold */
    CHECK(!ps_fallback_should_fallback(&one, 61000, 61000),
          "one over-threshold window -> no fallback");
    /* The next window is quiet: the streak resets, so the burst never trips. */
    CHECK(!ps_fallback_should_fallback(&one, 121000, 121000),
          "an isolated burst does not sustain");

    /* A sustained storm trips and latches. */
    ps_fallback_t fb;
    init(&fb);
    storm(&fb, 0, 25);          /* window 1: over */
    CHECK(!ps_fallback_should_fallback(&fb, 61000, 61000),
          "window 1 alone -> no fallback");
    storm(&fb, 61000, 25);      /* window 2: over */
    CHECK(!ps_fallback_should_fallback(&fb, 121000, 121000),
          "two windows -> still no fallback");
    storm(&fb, 121000, 25);     /* window 3: over */
    CHECK(ps_fallback_should_fallback(&fb, 181000, 181000),
          "three sustained windows -> fallback");
    CHECK(ps_fallback_is_latched(&fb), "fallback latches");

    /* Sticky: even a fresh connect does not un-latch. */
    ps_fallback_note_connected(&fb, 190000);
    CHECK(ps_fallback_should_fallback(&fb, 190000, 900000),
          "latched fallback stays true after a reconnect");

    /* An intervening quiet window breaks the streak. */
    ps_fallback_t reset;
    init(&reset);
    storm(&reset, 0, 25);       /* w1 over */
    CHECK(!ps_fallback_should_fallback(&reset, 61000, 61000), "reset w1");
    storm(&reset, 61000, 25);   /* w2 over */
    CHECK(!ps_fallback_should_fallback(&reset, 121000, 121000), "reset w2");
    /* w3 quiet -> the streak goes back to zero. */
    CHECK(!ps_fallback_should_fallback(&reset, 181000, 181000),
          "a quiet window resets the streak");
    storm(&reset, 181000, 25);  /* w4 over -> streak 1 */
    CHECK(!ps_fallback_should_fallback(&reset, 241000, 241000),
          "after the reset one window is not enough");
    storm(&reset, 241000, 25);  /* w5 over -> streak 2 */
    CHECK(!ps_fallback_should_fallback(&reset, 301000, 301000),
          "after the reset two windows are not enough");
    storm(&reset, 301000, 25);  /* w6 over -> streak 3 */
    CHECK(ps_fallback_should_fallback(&reset, 361000, 361000),
          "three windows after the reset trip");

    /* sustain_windows=1 keeps the old single-window behaviour. */
    ps_fallback_t fast;
    init_sustain(&fast, 1);
    storm(&fast, 0, 25);
    CHECK(ps_fallback_should_fallback(&fast, 61000, 61000),
          "sustain=1 trips on one over-threshold window");

    /* The uptime grace gates the fault even with a confirmed storm. */
    ps_fallback_t slow;
    init_sustain(&slow, 1);
    storm(&slow, 0, 25);
    CHECK(!ps_fallback_should_fallback(&slow, 61000, GRACE_MS - 1),
          "storm but still inside the grace -> no fallback");
    CHECK(ps_fallback_should_fallback(&slow, 61000, GRACE_MS),
          "storm past the grace -> fallback");

    /* An intentional reconnect is not counted. */
    ps_fallback_t intent;
    init(&intent);
    ps_fallback_note_connected(&intent, 1000);
    ps_fallback_note_intentional(&intent);
    ps_fallback_note_disconnect(&intent, 2000);
    CHECK(ps_fallback_disconnects(&intent) == 0,
          "an intentional disconnect is not counted");
    CHECK(!ps_fallback_should_fallback(&intent, 61000, 61000),
          "intentional reconnect -> no fallback");

    /* The window slides: disconnects spread wider than the window do not stack.
     * Two at t=1s/2s, then a third at t=200s - the first two have aged out. */
    ps_fallback_t slide;
    init(&slide);
    ps_fallback_note_disconnect(&slide, 1000);
    ps_fallback_note_disconnect(&slide, 2000);
    ps_fallback_note_disconnect(&slide, 200000);
    CHECK(!ps_fallback_should_fallback(&slide, 200000, 300000),
          "disconnects older than the window do not stack");
    CHECK(ps_fallback_disconnects(&slide) == 1, "only the fresh one remains");

    /* A stable association clears the count before it can reach the threshold. */
    ps_fallback_t stable;
    init(&stable);
    ps_fallback_note_disconnect(&stable, 1000);
    ps_fallback_note_disconnect(&stable, 2000);
    CHECK(ps_fallback_disconnects(&stable) == 2, "two drops before the stable run");
    ps_fallback_note_connected(&stable, 3000);
    /* Still associated 3 minutes later, then a single drop. */
    ps_fallback_note_disconnect(&stable, 3000 + STABLE_MS + 1000);
    CHECK(ps_fallback_disconnects(&stable) == 1,
          "a stable run cleared the earlier count");
    CHECK(!ps_fallback_should_fallback(&stable, 3000 + STABLE_MS + 1000, 400000),
          "no fallback after a stable run + one drop");

    /* NULL safety. */
    ps_fallback_note_disconnect(NULL, 0);
    ps_fallback_note_connected(NULL, 0);
    ps_fallback_note_intentional(NULL);
    CHECK(!ps_fallback_should_fallback(NULL, 0, 0), "NULL -> no fallback");
    CHECK(ps_fallback_disconnects(NULL) == 0, "NULL -> zero count");
    CHECK(!ps_fallback_is_latched(NULL), "NULL -> not latched");

    printf("ps fallback: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
