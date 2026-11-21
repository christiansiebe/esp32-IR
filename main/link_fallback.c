#include "link_fallback.h"

#include <stddef.h>

/* How long the proven mode stays enforced after a fallback fired, measured from
 * the fallback itself. Must comfortably cover: the peer noticing the link loss
 * (its own timeout), re-aligning (up to one realign interval) and a few of its
 * 5 s polls. 3x the silence timeout is ~3 min in the default config. */
#define LINK_FALLBACK_RECOVERY_WINDOW_MULT 3u

static uint64_t link_fallback_last_heard(const link_fallback_t *fb)
{
    return fb->last_rx_ms > fb->last_ok_ms ? fb->last_rx_ms : fb->last_ok_ms;
}

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
    fb->realign_interval_ms = 0;
    fb->last_realign_ms = started_at_ms;
    fb->fell_back_at_ms = 0;
    fb->heal_deadline_ms = 0;
    fb->last_heal_announce_ms = 0;
    fb->reboot_timeout_ms = 0;
    fb->last_reboot_check_ms = 0;
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

void link_fallback_set_realign_interval(link_fallback_t *fb, uint32_t interval_ms)
{
    if (fb != NULL) {
        fb->realign_interval_ms = interval_ms;
    }
}

void link_fallback_set_reboot_timeout(link_fallback_t *fb, uint32_t timeout_ms)
{
    if (fb != NULL) {
        fb->reboot_timeout_ms = timeout_ms;
    }
}

bool link_fallback_should_realign(link_fallback_t *fb, uint64_t now_ms,
                                  uint64_t uptime_ms)
{
    if (fb == NULL || fb->realign_interval_ms == 0) {
        return false;
    }
    if (uptime_ms < fb->min_uptime_ms) {
        return false;
    }
    /* Only the receive direction proves the peer can hear us. A broadcast send
     * reports success even into the void, so last_ok must not mask the silence -
     * otherwise a channel-stuck sender never re-scans. */
    if (now_ms < fb->last_rx_ms) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    if ((now_ms - fb->last_rx_ms) < fb->silence_timeout_ms) {
        return false; /* peer is answering: the channel is fine */
    }
    if (now_ms < fb->last_realign_ms ||
        (now_ms - fb->last_realign_ms) < fb->realign_interval_ms) {
        return false; /* rate-limit: at most one scan per interval */
    }
    fb->last_realign_ms = now_ms;
    return true;
}

bool link_fallback_should_fallback(link_fallback_t *fb, uint64_t now_ms,
                                   uint64_t uptime_ms)
{
    if (fb == NULL || fb->fell_back_at_ms != 0) {
        return false; /* already fallen back (or healed and ready to re-arm) */
    }
    if (uptime_ms < fb->min_uptime_ms) {
        return false;
    }
    /* The link is up while either direction shows life. Requiring both to go
     * quiet avoids tripping on a half-dead link that still carries a healthy
     * direction (e.g. unicast requests arrive but broadcasts do not). */
    uint64_t last_heard = link_fallback_last_heard(fb);
    if (now_ms < last_heard) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    if ((now_ms - last_heard) >= fb->silence_timeout_ms) {
        fb->fell_back_at_ms = now_ms;
        fb->heal_deadline_ms =
            now_ms + LINK_FALLBACK_RECOVERY_WINDOW_MULT * fb->silence_timeout_ms;
        fb->last_heal_announce_ms = 0;
        return true;
    }
    return false;
}

bool link_fallback_should_heal(link_fallback_t *fb, uint64_t now_ms)
{
    if (fb == NULL || fb->fell_back_at_ms == 0) {
        return false;
    }
    /* The recovery window keeps the proven mode applied long enough that a link
     * that just came back cannot re-trip the watchdog and flap; an answered
     * frame inside the window only re-arms the silence timer. */
    if (now_ms < fb->heal_deadline_ms) {
        return false;
    }
    /* Rate-limit the announcement: a 5 s poll must not fire it repeatedly while
     * the caller is mid-transition. */
    if (fb->last_heal_announce_ms != 0 &&
        (now_ms < fb->last_heal_announce_ms ||
         (now_ms - fb->last_heal_announce_ms) < fb->silence_timeout_ms)) {
        return false;
    }
    /* Only an actually answering peer counts as healed. */
    if (now_ms < fb->last_rx_ms ||
        (now_ms - fb->last_rx_ms) > fb->silence_timeout_ms) {
        return false; /* peer is still silent: nothing to heal */
    }
    fb->last_heal_announce_ms = now_ms;
    /* End the fallback state so a *fresh* silence episode can fall back again:
     * that is the repeatable self-heal (the one-shot stuck latch is gone). */
    fb->fell_back_at_ms = 0;
    fb->heal_deadline_ms = 0;
    return true;
}

bool link_fallback_should_reboot(link_fallback_t *fb, uint64_t now_ms)
{
    if (fb == NULL || fb->reboot_timeout_ms == 0 || fb->fell_back_at_ms == 0) {
        return false;
    }
    /* The peer answered after the fallback: the proven mode is working, no
     * reboot. (A heal will already have cleared fell_back_at_ms.) */
    uint64_t last_heard = link_fallback_last_heard(fb);
    if (now_ms < last_heard || (now_ms - last_heard) < fb->silence_timeout_ms) {
        return false;
    }
    if (now_ms < fb->fell_back_at_ms ||
        (now_ms - fb->fell_back_at_ms) < fb->reboot_timeout_ms) {
        return false; /* the post-fallback silence has not lasted long enough */
    }
    /* Rate-limit: at most one decision per silence timeout, so a fast poll cannot
     * double-fire while the caller is rebooting. */
    if (fb->last_reboot_check_ms != 0 &&
        (now_ms < fb->last_reboot_check_ms ||
         (now_ms - fb->last_reboot_check_ms) < fb->silence_timeout_ms)) {
        return false;
    }
    fb->last_reboot_check_ms = now_ms;
    return true;
}
