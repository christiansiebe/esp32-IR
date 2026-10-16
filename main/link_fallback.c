#include "link_fallback.h"

#include <stddef.h>

void link_fallback_init(link_fallback_t *fb, uint32_t silence_timeout_ms,
                        uint32_t min_uptime_ms, uint64_t started_at_ms)
{
    if (fb == NULL) {
        return;
    }
    fb->silence_timeout_ms = silence_timeout_ms;
    fb->min_uptime_ms = min_uptime_ms;
    fb->last_rx_ms = started_at_ms;
    fb->latched = false;
}

void link_fallback_note_rx(link_fallback_t *fb, uint64_t now_ms)
{
    if (fb != NULL) {
        fb->last_rx_ms = now_ms;
    }
}

bool link_fallback_should_fallback(link_fallback_t *fb, uint64_t now_ms,
                                   uint64_t uptime_ms)
{
    if (fb == NULL) {
        return false;
    }
    if (fb->latched) {
        return true; /* sticky: never un-fall-back without a fresh init */
    }
    if (uptime_ms < fb->min_uptime_ms) {
        return false;
    }
    if (now_ms < fb->last_rx_ms) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    if ((now_ms - fb->last_rx_ms) >= fb->silence_timeout_ms) {
        fb->latched = true;
        return true;
    }
    return false;
}
