/* Link fallback: each device declares the link down after the peer has been
 * silent past the timeout (and the uptime grace has passed), then returns to its
 * proven mode. The transition must be sticky so it cannot flap between the new
 * and the proven path. The *sender* judges only its outbound (acknowledged send)
 * clock, the *receiver* accepts either direction. Pure logic, no ESP-IDF. */
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

int main(void)
{
    /* Receiver (sender_role = false): silence counts from the last sign of life
     * in either direction (init seeds both clocks at boot). One ms short of the
     * timeout stays up; reaching it fires. */
    link_fallback_t fb;
    link_fallback_init(&fb, T_MS, 30000u, 0);
    CHECK(!link_fallback_should_fallback(&fb, T_MS - 1, T_MS - 1, false),
          "one ms short of the timeout -> no fallback");
    CHECK(link_fallback_should_fallback(&fb, T_MS, T_MS, false),
          "silence reaches the timeout -> fallback");

    /* The uptime grace gates the fallback even when the silence is long enough. */
    link_fallback_t slow_boot;
    link_fallback_init(&slow_boot, T_MS, 120000u, 0);
    CHECK(!link_fallback_should_fallback(&slow_boot, T_MS, T_MS, false),
          "silent but still inside the grace -> no fallback");
    CHECK(link_fallback_should_fallback(&slow_boot, 120000, 120000, false),
          "grace passed -> fallback");

    /* Sender (sender_role = true): only the acknowledged-send clock counts. A
     * sender whose peer never answers (a channel-stuck receiver) keeps sending
     * fine, so it must not disable its sleep. */
    link_fallback_t sender;
    link_fallback_init(&sender, T_MS, 0, 0);
    uint64_t ts = 0;
    bool sender_tripped = false;
    for (int i = 0; i < 1000; ++i) {
        ts += 1000;
        link_fallback_note_send_ok(&sender, ts);
        if (link_fallback_should_fallback(&sender, ts, ts, true)) {
            sender_tripped = true;
        }
    }
    CHECK(!sender_tripped, "acknowledged sends alone keep the link up");

    /* Regression: the sender's inbound path is legitimately quiet (the receiver
     * only talks on demand), so a stale last_rx with a fresh last_ok must NOT
     * trip the watchdog. This is exactly the on-device case that fired ~60 s
     * after boot while the sender was broadcasting every 5 s ("ok=1s rx=67s"). */
    link_fallback_t sender_rx_quiet;
    link_fallback_init(&sender_rx_quiet, T_MS, 0, 0);
    link_fallback_note_send_ok(&sender_rx_quiet, 100000); /* sent 1 s ago ... */
    CHECK(!link_fallback_should_fallback(&sender_rx_quiet, 101000, 101000, true),
          "sender: fresh send, stale receive -> no fallback");
    /* ... and with the send clock gone quiet it does fire. */
    CHECK(link_fallback_should_fallback(&sender_rx_quiet, 100000 + T_MS,
                                        100000 + T_MS, true),
          "sender: send clock quiet past the timeout -> fallback");

    /* The receiver, by contrast, still falls back when both clocks are stale
     * even though it pings (silence there means down). */
    link_fallback_t receiver_quiet;
    link_fallback_init(&receiver_quiet, T_MS, 0, 0);
    CHECK(link_fallback_should_fallback(&receiver_quiet, T_MS, T_MS, false),
          "receiver: both clocks stale -> fallback");

    /* A single inbound frame re-arms the receiver's silence timer. */
    link_fallback_t armed;
    link_fallback_init(&armed, T_MS, 0, 0);
    link_fallback_note_rx(&armed, 40000);
    CHECK(!link_fallback_should_fallback(&armed, 40000 + T_MS - 1, 40000 + T_MS - 1, false),
          "traffic just before the timeout -> no fallback");
    CHECK(link_fallback_should_fallback(&armed, 40000 + T_MS, 40000 + T_MS, false),
          "silence after the last frame -> fallback");

    /* Steady traffic never trips it, however long the uptime. */
    link_fallback_t busy;
    link_fallback_init(&busy, T_MS, 0, 0);
    uint64_t t = 0;
    bool tripped = false;
    for (int i = 0; i < 1000; ++i) {
        t += 5000;
        link_fallback_note_rx(&busy, t);
        if (link_fallback_should_fallback(&busy, t, t, false)) {
            tripped = true;
        }
    }
    CHECK(!tripped, "a frame every 5 s never trips the fallback");

    /* Sticky: once it fires it stays fired even after the peer returns, so the
     * proven mode is not dropped and re-armed in a loop. */
    link_fallback_t stuck;
    link_fallback_init(&stuck, T_MS, 0, 0);
    CHECK(link_fallback_should_fallback(&stuck, T_MS, T_MS, false), "fires once");
    link_fallback_note_rx(&stuck, T_MS + 1); /* peer answers again */
    CHECK(link_fallback_should_fallback(&stuck, T_MS + 2, T_MS + 2, false),
          "still fallen back after the peer returns (sticky)");

    /* Clock going backwards must not fire a bogus fallback. */
    link_fallback_t back;
    link_fallback_init(&back, T_MS, 0, 0);
    link_fallback_note_rx(&back, 100000);
    CHECK(!link_fallback_should_fallback(&back, 50000, 100000, false),
          "now before last_rx -> no fallback");

    /* NULL is safe. */
    CHECK(!link_fallback_should_fallback(NULL, 1, 1, true), "NULL state -> false");

    /* Keep-alive: the receiver pings once its own *outbound* traffic has been
     * quiet for half the silence timeout. This is the regression that made the
     * ping never fire on device: gating on the inbound clock (last_rx_ms) is
     * always fresh because the receiver hears the sender's broadcasts, so the
     * sender's watchdog tripped despite a healthy link. */
    CHECK(!link_keepalive_due(0, T_MS / 2 - 1, T_MS),
          "outbound quiet below half the timeout -> no ping yet");
    CHECK(link_keepalive_due(0, T_MS / 2, T_MS),
          "outbound quiet reaches half the timeout -> ping");
    CHECK(link_keepalive_due(1000, 1000 + T_MS, T_MS),
          "long outbound silence -> ping");
    CHECK(!link_keepalive_due(50000, 40000, T_MS),
          "now before last_ok -> no ping (clock went backwards)");
    CHECK(!link_keepalive_due(T_MS, T_MS + T_MS / 2 - 1, T_MS),
          "one ms before half the timeout -> no ping");

    printf("link fallback: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
