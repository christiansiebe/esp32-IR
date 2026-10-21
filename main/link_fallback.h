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
 * The sender additionally needs a *repeatable* channel re-align, not a sticky
 * one: the receiver's SoftAP follows the home AP's channel, which the router can
 * change at any time, and the sender's broadcast TX path reports success even
 * when nobody is listening. So a single boot-time scan (or a single fallback)
 * leaves the sender transmitting on a stale channel forever. While the peer is
 * silent the sender re-scans on a timer (see link_fallback_should_realign),
 * which keys on the *receive* direction only - last_ok must not mask it, or a
 * broadcast-only sender would never notice the silence.
 *
 * Pure C, no ESP-IDF, so the transition is host-tested (test/test_link_fallback.c).
 */

#include <stdbool.h>
#include <stdint.h>

/* What counts as the peer being alive. A link can be half-dead: the sender
 * transmits but nothing reaches the receiver (wrong channel) while the
 * receiver's requests still arrive because unicast uses a different channel
 * rule than broadcast. So a single "last heard" timestamp is ambiguous. Track
 * both directions: `last_rx` (peer heard) and `last_ok` (our own traffic to the
 * peer acknowledged). The proven modes both restore the send direction, and a
 * sender that fell silent because its *send* died would otherwise never trip the
 * watchdog (a channel-stuck receiver never answers anyway). */
typedef struct {
    uint32_t silence_timeout_ms; /* no healthy sign in either direction = down */
    uint32_t min_uptime_ms;      /* grace after boot before the link is judged */
    uint64_t last_rx_ms;         /* last inbound peer frame */
    uint64_t last_ok_ms;         /* last acknowledged send to the peer */
    uint32_t realign_interval_ms;/* min gap between channel re-align attempts (0 = off) */
    uint64_t last_realign_ms;    /* last re-align attempt, to rate-limit the scan */
    bool latched;                /* fell back; sticky until a fresh init */
} link_fallback_t;

/* Silence before the link is declared down. Long enough that a quiet wiring
 * path (no dashboard poll) does not trip it, short enough to recover quickly. */
#define LINK_FALLBACK_DEFAULT_SILENCE_MS (60u * 1000u)
/* Grace after boot: the peer may still be starting up. */
#define LINK_FALLBACK_DEFAULT_MIN_UPTIME_MS (30u * 1000u)

/* Initialise at boot; both clocks start at "now" so the grace window covers
 * startup. */
void link_fallback_init(link_fallback_t *fb, uint32_t silence_timeout_ms,
                        uint32_t min_uptime_ms, uint64_t started_at_ms);

/* A peer frame was received; re-arms the silence timer (no effect once fired). */
void link_fallback_note_rx(link_fallback_t *fb, uint64_t now_ms);

/* Our own send to the peer was acknowledged; also counts as the link being up.
 * Used by the sender, whose outbound path is what adaptive light sleep breaks. */
void link_fallback_note_send_ok(link_fallback_t *fb, uint64_t now_ms);

/* Enable the repeatable channel re-align: while the *receive* direction has been
 * silent for at least this long, link_fallback_should_realign() fires once per
 * interval so the caller can re-scan the receiver's AP channel. 0 disables it
 * (the receiver, which has no channel to chase, leaves it off). */
void link_fallback_set_realign_interval(link_fallback_t *fb, uint32_t interval_ms);

/* True when the peer has been silent long enough (receive direction only, so a
 * broadcast sender whose sends always "succeed" still notices) and the re-align
 * interval has elapsed since the last attempt. Each true advances the timer, so
 * a caller polling faster than the interval re-scans at most once per interval.
 * Never latches: the channel can go stale again after the router changes it. */
bool link_fallback_should_realign(link_fallback_t *fb, uint64_t now_ms,
                                  uint64_t uptime_ms);

/* True when the peer has been silent past the timeout and the uptime grace has
 * passed. Latches on first true (sticky) so the caller cannot flap. */
bool link_fallback_should_fallback(link_fallback_t *fb, uint64_t now_ms,
                                   uint64_t uptime_ms);
