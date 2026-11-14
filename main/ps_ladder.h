#pragma once

/* Adaptive Wi-Fi modem-sleep ladder (receiver).
 *
 * `ps max` parks the modem the longest and is the most frugal, but it is also
 * the most fragile: it wakes only every `listen_interval` beacons, so a burst
 * that lands outside a wake window is missed whole. `ps min` wakes at every
 * DTIM (more robust, a little more current), and `ps off` keeps the radio awake
 * (fully robust, the most current).
 *
 * This module runs the receiver on the *most frugal* mode it can hold and
 * automatically steps down when that costs data, then steps back up once the
 * link has been clean for a while - so the operator sets `ps max` and the
 * firmware finds the frugal-but-loss-free rung on its own, without a sticky
 * latch that would keep it at `off` forever.
 *
 * The signal is the ESP-NOW transport loss counter (forward sequence jumps =
 * a broadcast frame that never arrived). Too many losses in a window means the
 * current mode is dropping frames -> one rung down. A run of clean windows means
 * the rung holds -> one rung up (never above `start_mode`).
 *
 * Pure C, no ESP-IDF, so the ladder is host-tested (test/test_ps_ladder.c);
 * main.c feeds the loss deltas and performs the esp_wifi_set_ps() call.
 */

#include <stdbool.h>
#include <stdint.h>

/* Same numeric values as RUNTIME_CONFIG_PS_* so the caller can map 1:1. */
typedef enum {
    PS_LADDER_OFF = 0, /* WIFI_PS_NONE  - radio always awake */
    PS_LADDER_MIN = 1, /* WIFI_PS_MIN_MODEM - wake every DTIM */
    PS_LADDER_MAX = 2, /* WIFI_PS_MAX_MODEM - wake every listen_interval */
} ps_ladder_mode_t;

typedef struct {
    /* Configuration. */
    uint32_t window_ms;        /* loss counting / evaluation window */
    uint32_t loss_pct;         /* losses as a percentage of frames that trip */
    uint32_t min_frames;       /* don't judge a window with too few frames */
    uint32_t min_losses_to_step; /* ignore a lone stray loss below this */
    uint32_t recovery_windows; /* clean windows in a row to climb one rung */
    ps_ladder_mode_t floor_mode; /* never step below this (OFF or MIN) */
    ps_ladder_mode_t start_mode; /* the most frugal rung (MAX by default) */

    /* Observation state. */
    uint32_t window_start_ms;
    uint32_t losses;           /* losses seen in the current window */
    uint32_t frames;           /* frames seen in the current window */
    uint32_t clean_streak;     /* consecutive clean windows */
    uint8_t  step_failures[3]; /* times we stepped DOWN from each rung */
    ps_ladder_mode_t mode;     /* the rung currently in force */
    bool     enabled;

    /* Diagnostics (monotonic). */
    uint32_t total_losses;
    uint32_t steps_down;
    uint32_t steps_up;
} ps_ladder_t;

/* Defaults: a 60 s window (the sender's burst cycle); a window with at least
 * 4 frames and at least 2 losses at/over 20% loss costs a rung; 3 clean windows
 * (~3 min) climb one back; the floor is OFF so a genuinely lossy link can reach
 * the always-awake mode, and the ladder climbs back once it is clean again. */
#define PS_LADDER_DEFAULT_WINDOW_MS (60u * 1000u)
#define PS_LADDER_DEFAULT_LOSS_PCT 20u
#define PS_LADDER_DEFAULT_MIN_FRAMES 4u
#define PS_LADDER_DEFAULT_MIN_LOSSES 2u
#define PS_LADDER_DEFAULT_RECOVERY_WINDOWS 3u
#define PS_LADDER_DEFAULT_FLOOR PS_LADDER_OFF

/* A rung that cost us this many step-downs is treated as confirmed bad and is
 * not climbed back to for the rest of the session, so a persistently lossy rung
 * cannot oscillate (retry -> fail -> retry ...). One retry is allowed, which is
 * enough to recover from a single transient interference burst. */
#define PS_LADDER_MAX_RETRIES 2u

/* Initialise the ladder. `start_mode` is clamped to [floor_mode, MAX]; the
 * window clock starts at `started_at_ms` so the first window is measured from
 * boot. */
void ps_ladder_init(ps_ladder_t *l, uint32_t window_ms, uint32_t loss_pct,
                    uint32_t min_frames, uint32_t min_losses_to_step,
                    uint32_t recovery_windows, ps_ladder_mode_t floor_mode,
                    ps_ladder_mode_t start_mode, uint32_t started_at_ms);

/* Add newly observed losses/frames to the current window (the caller passes the
 * deltas of the transport counters since the last poll). */
void ps_ladder_note_loss(ps_ladder_t *l, uint32_t count);
void ps_ladder_note_frames(ps_ladder_t *l, uint32_t count);

/* Evaluate the window if it has elapsed and return the rung now in force. Call
 * periodically; the caller applies a changed return with esp_wifi_set_ps(). */
ps_ladder_mode_t ps_ladder_poll(ps_ladder_t *l, uint32_t now_ms);

/* The rung currently in force, without advancing time. */
ps_ladder_mode_t ps_ladder_mode(const ps_ladder_t *l);

/* Enable/disable adaptation (a manual 'set ps' may disable it so it does not
 * fight the operator). When disabled, poll() returns the mode unchanged. */
void ps_ladder_set_enabled(ps_ladder_t *l, bool on);
bool ps_ladder_enabled(const ps_ladder_t *l);

/* Force a rung (e.g. the operator set it explicitly) without counting it as a
 * step; resets the window and the clean streak. */
void ps_ladder_force_mode(ps_ladder_t *l, ps_ladder_mode_t mode, uint32_t now_ms);

/* Set a new start/ceiling rung (the operator changed `ps`): the ladder jumps to
 * it, clamps to the floor, and resets its window so the change is measured from
 * scratch. */
void ps_ladder_set_start(ps_ladder_t *l, ps_ladder_mode_t start_mode,
                         uint32_t now_ms);

/* Diagnostics. */
uint32_t ps_ladder_window_losses(const ps_ladder_t *l);
uint32_t ps_ladder_total_losses(const ps_ladder_t *l);
uint32_t ps_ladder_steps_down(const ps_ladder_t *l);
uint32_t ps_ladder_steps_up(const ps_ladder_t *l);
