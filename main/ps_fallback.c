#include "ps_fallback.h"

#include <stddef.h>

void ps_fallback_init(ps_fallback_t *fb, uint32_t window_ms, uint32_t threshold,
                      uint32_t min_uptime_ms, uint32_t stable_reset_ms,
                      uint64_t started_at_ms)
{
    if (fb == NULL) {
        return;
    }
    fb->window_ms = window_ms;
    fb->threshold = threshold;
    fb->min_uptime_ms = min_uptime_ms;
    fb->stable_reset_ms = stable_reset_ms;
    fb->window_start_ms = started_at_ms;
    fb->disconnects = 0;
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
        fb->window_start_ms = now_ms;
        fb->disconnects = 0;
    }
    fb->disconnects++;
}

bool ps_fallback_should_fallback(ps_fallback_t *fb, uint64_t now_ms,
                                 uint64_t uptime_ms)
{
    (void)now_ms;
    if (fb == NULL) {
        return false;
    }
    if (fb->latched) {
        return true; /* sticky: never un-fall-back without a fresh init */
    }
    if (uptime_ms < fb->min_uptime_ms) {
        return false;
    }
    if (fb->disconnects >= fb->threshold) {
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
