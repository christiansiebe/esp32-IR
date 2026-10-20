#include "net_watchdog.h"

#include <string.h>

void net_wd_default_policy(net_wd_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }
    policy->down_timeout_ms = 120000;   /* 2 min */
    policy->reboot_timeout_ms = 300000; /* 5 min after the reconnect attempt */
    policy->min_uptime_ms = 120000;     /* 2 min grace after boot */
}

void net_wd_init(net_wd_t *w, const net_wd_policy_t *policy, uint64_t now_ms)
{
    if (w == NULL) {
        return;
    }
    memset(w, 0, sizeof(*w));
    if (policy != NULL) {
        w->policy = *policy;
    }
    w->started_at_ms = now_ms;
}

void net_wd_note_up(net_wd_t *w, uint64_t now_ms)
{
    (void)now_ms;
    if (w == NULL) {
        return;
    }
    w->down_since_ms = 0;
    w->reconnect_tried = false;
    w->reboot_latched = false;
}

net_wd_action_t net_wd_tick(net_wd_t *w, uint64_t now_ms, bool up)
{
    if (w == NULL) {
        return NET_WD_OK;
    }
    /* Once a reboot is decided it stays decided: the caller is about to reset
     * the device, and a late "up" must not cancel it mid-flight. */
    if (w->reboot_latched) {
        return NET_WD_REBOOT;
    }
    if (up) {
        net_wd_note_up(w, now_ms);
        return NET_WD_OK;
    }
    /* Never judge the link before the boot grace has passed: association and
     * DHCP take a moment, and a slow router boot must not look like an outage. */
    if (now_ms - w->started_at_ms < w->policy.min_uptime_ms) {
        return NET_WD_OK;
    }
    if (w->down_since_ms == 0) {
        w->down_since_ms = now_ms;
        return NET_WD_OK;
    }
    uint64_t down_ms = now_ms - w->down_since_ms;
    if (!w->reconnect_tried) {
        if (down_ms >= w->policy.down_timeout_ms) {
            w->reconnect_tried = true;
            return NET_WD_RECONNECT;
        }
        return NET_WD_OK;
    }
    /* The reconnect window is measured from the same down edge, so the reboot
     * fires down_timeout + reboot_timeout after the link first went down. */
    if (down_ms >= (uint64_t)w->policy.down_timeout_ms + w->policy.reboot_timeout_ms) {
        w->reboot_latched = true;
        return NET_WD_REBOOT;
    }
    return NET_WD_OK;
}
