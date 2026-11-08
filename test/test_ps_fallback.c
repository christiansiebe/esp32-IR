/* Wi-Fi modem-sleep fault detector: too many STA disconnects inside the window
 * while a PS mode is active must fall back (sticky) to the proven mode, an
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

#define WIN_MS 120000u
#define THRESH 3u
#define GRACE_MS 30000u
#define STABLE_MS 120000u

static void init(ps_fallback_t *fb)
{
    ps_fallback_init(fb, WIN_MS, THRESH, GRACE_MS, STABLE_MS, 0);
}

int main(void)
{
    /* Two disconnects in the window are not yet a storm. */
    ps_fallback_t fb;
    init(&fb);
    ps_fallback_note_connected(&fb, 1000);
    ps_fallback_note_disconnect(&fb, 2000);
    ps_fallback_note_connected(&fb, 2500);
    ps_fallback_note_disconnect(&fb, 3000);
    CHECK(!ps_fallback_should_fallback(&fb, 3000, 31000),
          "two disconnects -> no fallback");
    CHECK(ps_fallback_disconnects(&fb) == 2, "two disconnects counted");

    /* The third inside the window trips it. */
    ps_fallback_note_connected(&fb, 3500);
    ps_fallback_note_disconnect(&fb, 4000);
    CHECK(ps_fallback_should_fallback(&fb, 4000, 31000),
          "three disconnects -> fallback");
    CHECK(ps_fallback_is_latched(&fb), "fallback latches");

    /* Sticky: even a fresh connect does not un-latch. */
    ps_fallback_note_connected(&fb, 5000);
    CHECK(ps_fallback_should_fallback(&fb, 5000, 900000),
          "latched fallback stays true after a reconnect");

    /* The uptime grace gates the fault even with a storm. */
    ps_fallback_t slow;
    init(&slow);
    ps_fallback_note_disconnect(&slow, 1000);
    ps_fallback_note_disconnect(&slow, 2000);
    ps_fallback_note_disconnect(&slow, 3000);
    CHECK(!ps_fallback_should_fallback(&slow, 3000, GRACE_MS - 1),
          "storm but still inside the grace -> no fallback");
    CHECK(ps_fallback_should_fallback(&slow, 3000, GRACE_MS),
          "storm past the grace -> fallback");

    /* An intentional reconnect is not counted. */
    ps_fallback_t intent;
    init(&intent);
    ps_fallback_note_connected(&intent, 1000);
    ps_fallback_note_intentional(&intent);
    ps_fallback_note_disconnect(&intent, 2000);
    CHECK(ps_fallback_disconnects(&intent) == 0,
          "an intentional disconnect is not counted");
    CHECK(!ps_fallback_should_fallback(&intent, 2000, 31000),
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
