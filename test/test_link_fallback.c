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

    /* Own acknowledged sends count too: a sender whose peer never answers (a
     * channel-stuck receiver) must not disable its sleep. */
    link_fallback_t sender;
    link_fallback_init(&sender, T_MS, 0, 0);
    uint64_t ts = 0;
    bool sender_tripped = false;
    for (int i = 0; i < 1000; ++i) {
        ts += 1000;
        link_fallback_note_send_ok(&sender, ts);
        if (link_fallback_should_fallback(&sender, ts, ts)) {
            sender_tripped = true;
        }
    }
    CHECK(!sender_tripped, "acknowledged sends alone keep the link up");

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

    /* --- Periodic channel re-align (sender) ------------------------------- */

    /* Off by default: a receiver that never calls the setter must not re-scan. */
    link_fallback_t off;
    link_fallback_init(&off, T_MS, 0, 0);
    CHECK(!link_fallback_should_realign(&off, 10u * T_MS, 10u * T_MS),
          "re-align disabled -> never fires");

    /* While the peer keeps answering, the channel is fine and no scan runs. */
    link_fallback_t live;
    link_fallback_init(&live, T_MS, 0, 0);
    link_fallback_set_realign_interval(&live, T_MS);
    bool live_realigned = false;
    for (uint64_t t = 0; t <= 20u * T_MS; t += 5000) {
        link_fallback_note_rx(&live, t);
        if (link_fallback_should_realign(&live, t, t)) {
            live_realigned = true;
        }
    }
    CHECK(!live_realigned, "traffic every 5 s -> no re-align");

    /* A broadcast sender's own sends always "succeed" into the void; they must
     * not mask the silence, or the stale channel is never re-scanned. */
    link_fallback_t stuck_sender;
    link_fallback_init(&stuck_sender, T_MS, 0, 0);
    link_fallback_set_realign_interval(&stuck_sender, T_MS);
    uint64_t st = 0;
    bool masked = false;
    for (int i = 0; i < 100; ++i) {
        st += 5000;
        link_fallback_note_send_ok(&stuck_sender, st);
        if (link_fallback_should_realign(&stuck_sender, st, st)) {
            masked = true;
        }
    }
    CHECK(masked, "silent peer re-aligns even while sends report success");

    /* Rate-limited: polling every 5 s re-scans once per interval, not once per
     * poll. Count the fires over a window and compare to the interval. */
    link_fallback_t paced;
    link_fallback_init(&paced, T_MS, 0, 0);
    link_fallback_set_realign_interval(&paced, 60000u);
    int fires = 0;
    uint64_t pt = 0;
    for (int i = 0; i < 120; ++i) { /* 120 * 5 s = 10 min */
        pt += 5000;
        if (link_fallback_should_realign(&paced, pt, pt)) {
            ++fires;
        }
    }
    /* First fire at T_MS, then every 60 s for the remaining ~9 min -> ~10. */
    CHECK(fires >= 8 && fires <= 11, "one scan per interval, not per poll");

    /* The uptime grace gates the first scan too. */
    link_fallback_t boot_grace;
    link_fallback_init(&boot_grace, T_MS, 120000u, 0);
    link_fallback_set_realign_interval(&boot_grace, T_MS);
    CHECK(!link_fallback_should_realign(&boot_grace, T_MS, T_MS),
          "silent but inside the grace -> no re-align");
    CHECK(link_fallback_should_realign(&boot_grace, 120000, 120000),
          "grace passed -> re-align");

    /* A frame arriving re-arms the re-align silence window (a re-align that
     * fixed the channel must not keep scanning every interval). */
    link_fallback_t healed;
    link_fallback_init(&healed, T_MS, 0, 0);
    link_fallback_set_realign_interval(&healed, T_MS);
    CHECK(link_fallback_should_realign(&healed, T_MS, T_MS), "fires while silent");
    link_fallback_note_rx(&healed, T_MS + 1); /* channel fixed, peer answers */
    CHECK(!link_fallback_should_realign(&healed, T_MS + T_MS - 1, T_MS + T_MS - 1),
          "answered peer -> no further re-align");

    /* The clock going backwards must not fire a re-align. */
    link_fallback_t rewind;
    link_fallback_init(&rewind, T_MS, 0, 0);
    link_fallback_set_realign_interval(&rewind, T_MS);
    link_fallback_note_rx(&rewind, 100000);
    CHECK(!link_fallback_should_realign(&rewind, 50000, 100000),
          "now before last_rx -> no re-align");

    CHECK(!link_fallback_should_realign(NULL, 1, 1), "NULL re-align -> false");

    printf("link fallback: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
