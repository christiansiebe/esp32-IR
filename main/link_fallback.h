#pragma once

/* Per-device "the other side stopped answering" fallback.
 *
 * Both roles already have a local watchdog for their own health (web_guard on
 * the receiver). This is the missing half: each device watches the *link* and,
 * when the peer has been silent for too long, falls back to its proven mode.
 * It is deliberately one-sided per device and needs no handshake, so the two
 * sides can never deadlock waiting for each other:
 *
 *   Sender:   fell silent while the adaptive light sleep was on -> turn the
 *             sleep off and keep the Wi-Fi radio awake (the proven mode).
 *   Receiver: fell silent -> surface it in the diagnostics; a hung *receiver*
 *             is already caught by web_guard, so no reboot is needed here.
 *
 * The decision is sticky on purpose: once the new path is dropped it stays
 * dropped until the next boot. Re-arming it after a single answered poll would
 * flap (sleep off -> peer answers -> sleep on -> silence -> ...).
 *
 * Pure C, no ESP-IDF, so the transition is host-tested (test/test_link_fallback.c).
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t silence_timeout_ms; /* no peer traffic for this long = link down */
    uint32_t min_uptime_ms;      /* grace after boot before the link is judged */
    uint64_t last_rx_ms;         /* last inbound peer frame */
    bool latched;                /* fell back; sticky until a fresh init */
} link_fallback_t;

/* Silence before the link is declared down. Long enough that a quiet wiring
 * path (no dashboard poll) does not trip it, short enough to recover quickly. */
#define LINK_FALLBACK_DEFAULT_SILENCE_MS (60u * 1000u)
/* Grace after boot: the peer may still be starting up. */
#define LINK_FALLBACK_DEFAULT_MIN_UPTIME_MS (30u * 1000u)

/* Initialise at boot; last_rx is "now" so the grace window covers startup. */
void link_fallback_init(link_fallback_t *fb, uint32_t silence_timeout_ms,
                        uint32_t min_uptime_ms, uint64_t started_at_ms);

/* A peer frame was received; re-arms the silence timer (no effect once fired). */
void link_fallback_note_rx(link_fallback_t *fb, uint64_t now_ms);

/* True when the peer has been silent past the timeout and the uptime grace has
 * passed. Latches on first true (sticky) so the caller cannot flap. */
bool link_fallback_should_fallback(link_fallback_t *fb, uint64_t now_ms,
                                   uint64_t uptime_ms);
