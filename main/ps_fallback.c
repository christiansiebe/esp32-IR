#include "ps_fallback.h"

#include <stddef.h>

/* Close the current window: a window at/over the threshold extends the
 * over-threshold streak, a window under it breaks the streak. The window is then
 * restarted. Shared by the disconnect feed and the poll so a window is evaluated
 * exactly once regardless of which one crosses the boundary first. */
static void ps_fallback_close_window(ps_fallback_t *fb, uint64_t now_ms)
{
    if (fb->disconnects >= fb->threshold) {
        fb->over_threshold_windows++;
    } else {
        fb->over_threshold_windows = 0;
    }
    fb->window_start_ms = now_ms;
    fb->disconnects = 0;
}

void ps_fallback_init(ps_fallback_t *fb, uint32_t window_ms, uint32_t threshold,
                      uint32_t sustain_windows, uint32_t min_uptime_ms,
                      uint32_t stable_reset_ms, uint64_t started_at_ms)
{
    if (fb == NULL) {
        return;
    }
    fb->window_ms = window_ms;
    fb->threshold = threshold;
    fb->sustain_windows = sustain_windows == 0 ? 1u : sustain_windows;
    fb->min_uptime_ms = min_uptime_ms;
    fb->stable_reset_ms = stable_reset_ms;
    fb->window_start_ms = started_at_ms;
    fb->disconnects = 0;
    fb->over_threshold_windows = 0;
    fb->connected_since_ms = 0;
    fb->pending_intentional = false;
    fb->latched = false;
}

void ps_fallback_note_connected(ps_fallback_t *fb, uint64_t now_ms)
{
    if (fb != NULL) {
        fb->connected_since_ms = now_ms;
    }
}

void ps_fallback_note_intentional(ps_fallback_t *fb)
{
    if (fb != NULL) {
        fb->pending_intentional = true;
    }
}

void ps_fallback_note_disconnect(ps_fallback_t *fb, uint64_t now_ms)
{
    if (fb == NULL || fb->latched) {
        return;
    }
    if (fb->pending_intentional) {
        /* A 'set ps'/'set wifi-*' reconnect is expected, not a fault. */
        fb->pending_intentional = false;
        return;
    }
    /* A long, stable association clears the count: the PS mode proved it can
     * hold the link, so an old drop must not accumulate towards a false trip. */
    if (fb->connected_since_ms != 0 && now_ms >= fb->connected_since_ms &&
        (now_ms - fb->connected_since_ms) >= fb->stable_reset_ms) {
        fb->window_start_ms = now_ms;
        fb->disconnects = 0;
    }
    fb->connected_since_ms = 0; /* the association is gone */

    if (now_ms < fb->window_start_ms ||
        (now_ms - fb->window_start_ms) >= fb->window_ms) {
        ps_fallback_close_window(fb, now_ms);
    }
    fb->disconnects++;
}

bool ps_fallback_should_fallback(ps_fallback_t *fb, uint64_t now_ms,
                                 uint64_t uptime_ms)
{
    if (fb == NULL) {
        return false;
    }
    if (fb->latched) {
        return true; /* sticky: never un-fall-back without a fresh init */
    }
    /* Evaluate a window as soon as it has elapsed, so a window that was over the
     * threshold is banked even if no further disconnect arrives to roll it. */
    if (now_ms >= fb->window_start_ms &&
        (now_ms - fb->window_start_ms) >= fb->window_ms) {
        ps_fallback_close_window(fb, now_ms);
    }
    if (uptime_ms < fb->min_uptime_ms) {
        return false;
    }
    if (fb->over_threshold_windows >= fb->sustain_windows) {
        fb->latched = true;
        return true;
    }
    return false;
}

uint32_t ps_fallback_disconnects(const ps_fallback_t *fb)
{
    return fb != NULL ? fb->disconnects : 0;
}

bool ps_fallback_is_latched(const ps_fallback_t *fb)
{
    return fb != NULL && fb->latched;
}
