/* Link fallback: each device declares the link down after the peer has been
 * silent past the timeout (and the uptime grace has passed), then returns to its
 * proven mode. The transition must be sticky so it cannot flap between the new
 * and the proven path. Pure logic, no ESP-IDF. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "link_fallback.h"

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

#define T_MS 60000u
#define GRACE_MS 30000u

int main(void)
{
    /* Silence counts from the last inbound frame (init seeds it at boot). One ms
     * short of the timeout stays up; reaching it fires. */
    link_fallback_t fb;
    link_fallback_init(&fb, T_MS, 30000u, 0);
    CHECK(!link_fallback_should_fallback(&fb, T_MS - 1, T_MS - 1),
          "one ms short of the timeout -> no fallback");
    CHECK(link_fallback_should_fallback(&fb, T_MS, T_MS),
          "silence reaches the timeout -> fallback");

    /* The uptime grace gates the fallback even when the silence is long enough. */
    link_fallback_t slow_boot;
    link_fallback_init(&slow_boot, T_MS, 120000u, 0);
    CHECK(!link_fallback_should_fallback(&slow_boot, T_MS, T_MS),
          "silent but still inside the grace -> no fallback");
    CHECK(link_fallback_should_fallback(&slow_boot, 120000, 120000),
          "grace passed -> fallback");

    /* A single inbound frame re-arms the silence timer. */
    link_fallback_t armed;
    link_fallback_init(&armed, T_MS, 0, 0);
    link_fallback_note_rx(&armed, 40000);
    CHECK(!link_fallback_should_fallback(&armed, 40000 + T_MS - 1, 40000 + T_MS - 1),
          "traffic just before the timeout -> no fallback");
    CHECK(link_fallback_should_fallback(&armed, 40000 + T_MS, 40000 + T_MS),
          "silence after the last frame -> fallback");

    /* Steady traffic never trips it, however long the uptime. */
    link_fallback_t busy;
    link_fallback_init(&busy, T_MS, 0, 0);
    uint64_t t = 0;
    bool tripped = false;
    for (int i = 0; i < 1000; ++i) {
        t += 5000;
        link_fallback_note_rx(&busy, t);
        if (link_fallback_should_fallback(&busy, t, t)) {
            tripped = true;
        }
    }
    CHECK(!tripped, "a frame every 5 s never trips the fallback");

    /* Sticky: once it fires it stays fired even after the peer returns, so the
     * proven mode is not dropped and re-armed in a loop. */
    link_fallback_t stuck;
    link_fallback_init(&stuck, T_MS, 0, 0);
    CHECK(link_fallback_should_fallback(&stuck, T_MS, T_MS), "fires once");
    link_fallback_note_rx(&stuck, T_MS + 1); /* peer answers again */
    CHECK(link_fallback_should_fallback(&stuck, T_MS + 2, T_MS + 2),
          "still fallen back after the peer returns (sticky)");

    /* Clock going backwards must not fire a bogus fallback. */
    link_fallback_t back;
    link_fallback_init(&back, T_MS, 0, 0);
    link_fallback_note_rx(&back, 100000);
    CHECK(!link_fallback_should_fallback(&back, 50000, 100000),
          "now before last_rx -> no fallback");

    /* NULL is safe. */
    CHECK(!link_fallback_should_fallback(NULL, 1, 1), "NULL state -> false");

    printf("link fallback: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
