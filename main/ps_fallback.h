#pragma once

/* Wi-Fi modem-sleep (power-save) fault detector + fallback.
 *
 * Modem sleep (WIFI_PS_MIN_MODEM / WIFI_PS_MAX_MODEM) is the only knob that
 * really cuts the C6 receiver's current, but it is link-risky: the driver parks
 * the radio between DTIM beacons, and on some APs (or with a too-large
 * listen_interval) the station then loses beacons and re-associates over and
 * over. That failure is visible as a *storm of disconnects* right after the
 * mode is applied, not as a crash - so it needs its own detector.
 *
 * The other two guards cover different halves and do not see it:
 *   - runtime_config_verify()/repair() catches a mode the *driver* rejects.
 *   - link_fallback watches the ESP-NOW peer (the sender side).
 *   - liveness_guard catches a *global* stall.
 * None of them notices "the station keeps dropping because of the PS mode".
 *
 * This module does: it counts STA disconnects inside a sliding window while a
 * modem PS mode is active. Too many in the window means the PS mode is breaking
 * the link, so the caller drops to the proven mode (WIFI_PS_NONE, radio always
 * awake) and latches it for the rest of the session (sticky, so it cannot
 * flap: PS off -> link recovers -> PS on -> drops again). An intentional
 * disconnect (a 'set ps'/'set wifi-*' reconnect) is excluded so a deliberate
 * re-association is never counted as a fault.
 *
 * Pure C, no ESP-IDF, so the decision is host-tested (test/test_ps_fallback.c).
 * main.c performs the event feed and the actual esp_wifi_set_ps() call.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t window_ms;        /* sliding window the disconnects are counted in */
    uint32_t threshold;        /* disconnects in the window that trip the fault */
    uint32_t sustain_windows;  /* over-threshold windows in a row needed to trip */
    uint32_t min_uptime_ms;    /* grace after boot before a fault may fire */
    uint32_t stable_reset_ms;  /* connected this long clears the window count */
    uint64_t window_start_ms;  /* start of the current counting window */
    uint32_t disconnects;      /* disconnects counted in the current window */
    uint32_t over_threshold_windows; /* consecutive over-threshold windows seen */
    uint64_t connected_since_ms; /* start of the current association (0 = down) */
    bool pending_intentional;  /* the next disconnect is an expected reconnect */
    bool latched;              /* fell back; sticky until a fresh init */
} ps_fallback_t;

/* Defaults: a *sustained* storm is the fault. One window over the threshold is
 * only a candidate; a real modem-sleep fault keeps the link down, so the next
 * window is over the threshold again. An intermittent ambient storm (the
 * FRITZ!Box mesh's reason-34 re-associations, which happen with power save off
 * too) is over by the next window and never sustains. */
#define PS_FALLBACK_DEFAULT_WINDOW_MS (60u * 1000u)
#define PS_FALLBACK_DEFAULT_THRESHOLD 20u
#define PS_FALLBACK_DEFAULT_SUSTAIN_WINDOWS 3u
#define PS_FALLBACK_DEFAULT_MIN_UPTIME_MS (30u * 1000u)
#define PS_FALLBACK_DEFAULT_STABLE_RESET_MS (120u * 1000u)

/* Initialise at boot; both clocks start at `started_at_ms` so the uptime grace
 * covers startup. `sustain_windows` is the number of consecutive over-threshold
 * windows that must be seen before the fault latches (>=1; 1 keeps the old
 * single-window behaviour). */
void ps_fallback_init(ps_fallback_t *fb, uint32_t window_ms, uint32_t threshold,
                      uint32_t sustain_windows, uint32_t min_uptime_ms,
                      uint32_t stable_reset_ms, uint64_t started_at_ms);

/* The station associated. Records the association start so a long, stable
 * association can clear the disconnect count (the PS mode proved itself). */
void ps_fallback_note_connected(ps_fallback_t *fb, uint64_t now_ms);

/* The station disconnected. A pending intentional marker (see below) consumes
 * this event instead of counting it. Counts within the sliding window and may
 * latch the fault. No effect once latched. */
void ps_fallback_note_disconnect(ps_fallback_t *fb, uint64_t now_ms);

/* Mark the *next* disconnect as an expected reconnect (a deliberate 'set ps' /
 * 'set wifi-*' / reset-config), so it is not counted as a fault. */
void ps_fallback_note_intentional(ps_fallback_t *fb);

/* True when the disconnect storm *sustained* across `sustain_windows`
 * consecutive windows and the uptime grace has passed. A window over the
 * threshold is counted and the window restarted; an intervening window back
 * under the threshold resets the streak, so an isolated ambient burst never
 * trips. Latches on the first true so the caller cannot flap. */
bool ps_fallback_should_fallback(ps_fallback_t *fb, uint64_t now_ms,
                                 uint64_t uptime_ms);

/* Diagnostics: the current window disconnect count and the latch. */
uint32_t ps_fallback_disconnects(const ps_fallback_t *fb);
bool ps_fallback_is_latched(const ps_fallback_t *fb);
