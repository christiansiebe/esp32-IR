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
                                   uint64_t uptime_ms, bool sender_role)
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
    /* What counts as "the link is up" depends on the role:
     *
     * - Sender: only its *outbound* path matters. The receiver talks to it on
     *   demand (dashboard poll, hourly time sync, history sync), so the sender's
     *   inbound path is legitimately silent for long stretches even on a
     *   perfectly healthy link. Judging the link by the inbound clock made the
     *   watchdog fire ~60 s after every boot on a link that was sending fine
     *   (the fallback event logged "ok=1s rx=67s"). The fallback exists to fix a
     *   broken *send* path (light sleep dropping frames / wrong channel), so the
     *   acknowledged-send clock is the right signal.
     *
     * - Receiver: either direction counts. It has no reason to stop sending to
     *   the sender (it pings to keep the sender's watchdog fed), so a stale
     *   inbound clock there really does mean the link is down. */
    uint64_t last_heard = sender_role
                              ? fb->last_ok_ms
                              : (fb->last_rx_ms > fb->last_ok_ms ? fb->last_rx_ms
                                                                 : fb->last_ok_ms);
    if (now_ms < last_heard) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    if ((now_ms - last_heard) >= fb->silence_timeout_ms) {
        fb->latched = true;
        return true;
    }
    return false;
}

bool link_keepalive_due(uint64_t last_ok_ms, uint64_t now_ms,
                        uint32_t silence_timeout_ms)
{
    if (now_ms < last_ok_ms) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    return (now_ms - last_ok_ms) >= (uint64_t)silence_timeout_ms / 2u;
}
