#pragma once

/* Burst-locked receive window (receiver side).
 *
 * The sender does not emit one ESP-NOW packet per telegram: it drains its ring
 * once per SML_TX_CYCLE_MS as a short burst (a handful of frames a few ms
 * apart). The receiver's own ESP-NOW wake window is scheduled by the Wi-Fi
 * driver (its phase is fixed there, e.g. TBTT-aligned while the station is
 * associated) and cannot be pointed at the sender's burst. A window that is
 * short compared to the sender's cycle therefore drifts against the burst and
 * misses whole bursts - exactly the loss measured on the bench.
 *
 * This module removes the drift by predicting the burst instead of scheduling
 * against it: it watches when bursts actually arrive (in the receiver's own
 * millisecond clock), estimates the burst period, and reports when the next
 * receive window should open so the caller can wake the modem (WIFI_PS_NONE)
 * just before the predicted burst and park it again afterwards. Because the
 * prediction is re-anchored on every burst the receiver catches, it tracks the
 * sender without any shared clock, and if it ever loses the sender it falls
 * back to "stay awake and re-learn" rather than missing data silently.
 *
 * Pure C (no ESP-IDF), so it is host-tested (test/test_burst_lock.c).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Configuration. */
    uint32_t gap_ms;        /* frames closer than this belong to one burst */
    uint32_t window_ms;     /* how long the receive window stays open */
    uint32_t lead_ms;       /* open this early before the predicted burst */
    uint32_t min_period_ms; /* plausible burst period, lower bound */
    uint32_t max_period_ms; /* plausible burst period, upper bound */
    uint32_t max_misses;    /* consecutive empty windows before re-learning */

    /* Observation state. */
    bool     have_burst;    /* a burst start has been seen */
    bool     locked;        /* period known, prediction usable */
    bool     in_window;     /* the receive window is currently open */
    bool     window_served; /* a frame arrived while in_window */
    uint32_t last_frame_ms; /* arrival of the most recent frame */
    uint32_t last_burst_ms; /* start of the most recent burst */
    uint32_t period_ms;     /* estimated burst period (0 = unknown) */
    uint32_t misses;        /* consecutive empty windows */

    /* Counters (diagnostics). */
    uint32_t bursts;
    uint32_t locks;
    uint32_t unlocks;
    uint32_t window_hits;
    uint32_t window_misses;
} burst_lock_t;

/* Initialise with the given timings. A window_ms of 0 falls back to 400 ms, a
 * gap_ms of 0 to 1000 ms, and lead_ms is capped below window_ms. max_misses 0
 * falls back to 3. The period is bounded to [1000, 20000] ms by default. */
void burst_lock_init(burst_lock_t *bl, uint32_t gap_ms, uint32_t window_ms,
                     uint32_t lead_ms, uint32_t max_misses);

/* Feed one inbound burst frame's arrival time (ms, monotonic). Returns true when
 * the frame started a new burst. Re-anchors the prediction and marks the current
 * window as served when the frame lands inside it. */
bool burst_lock_note_frame(burst_lock_t *bl, uint32_t now_ms);

/* True once a period has been learned and a prediction is usable. */
bool burst_lock_locked(const burst_lock_t *bl);

/* Milliseconds from now until the next window should open (0 = open now). While
 * not locked the caller should stay awake and keep observing; the return is then
 * 0 as well, and burst_lock_locked() tells the two cases apart. */
int32_t burst_lock_wait_ms(const burst_lock_t *bl, uint32_t now_ms);

/* Open the window and clear the per-window "served" flag. */
void burst_lock_window_open(burst_lock_t *bl);

/* While the window is open, decide whether to keep it open past the nominal
 * window_ms because frames are still arriving: a burst that now spans the
 * sender's ACK round trips can run longer than one window, and cutting it off
 * would lose the tail. `open_ms` is when the window was opened, `now_ms` the
 * current time, `max_open_ms` a hard cap so a runaway cannot hold the radio
 * awake forever. Returns false once the burst has gone quiet or the cap is hit. */
bool burst_lock_window_should_extend(const burst_lock_t *bl, uint32_t open_ms,
                                     uint32_t now_ms, uint32_t max_open_ms);

/* Close the window: a served window resets the miss count; an empty one counts
 * up and, after max_misses in a row, drops back to re-learning (unlocked). */
void burst_lock_window_close(burst_lock_t *bl);

#ifdef __cplusplus
}
#endif
