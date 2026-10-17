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
    fb->last_ok_ms = started_at_ms;
    fb->latched = false;
}

void link_fallback_note_rx(link_fallback_t *fb, uint64_t now_ms)
{
    if (fb != NULL) {
        fb->last_rx_ms = now_ms;
    }
}

void link_fallback_note_send_ok(link_fallback_t *fb, uint64_t now_ms)
{
    if (fb != NULL) {
        fb->last_ok_ms = now_ms;
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
    /* The link is up while either direction shows life. Requiring both to go
     * quiet avoids tripping on a half-dead link that still carries a healthy
     * direction (e.g. unicast requests arrive but broadcasts do not). */
    uint64_t last_heard = fb->last_rx_ms > fb->last_ok_ms ? fb->last_rx_ms
                                                          : fb->last_ok_ms;
    if (now_ms < last_heard) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    if ((now_ms - last_heard) >= fb->silence_timeout_ms) {
        fb->latched = true;
        return true;
    }
    return false;
}
