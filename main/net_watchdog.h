/* Receiver Wi-Fi link watchdog.
 *
 * The receiver's existing guards each cover one path: web_guard restarts the
 * HTTP server, health_task reboots on stale SML data, liveness_guard catches a
 * global stall. None of them notices a lost Wi-Fi link. The station can drop
 * its association (router reboot, channel change/radar, DHCP lease loss, a
 * wedged driver) while every task keeps running and the HTTP server keeps
 * answering on the loopback. The dashboard is then unreachable from the home
 * LAN - the device is "offline" without a single watchdog firing.
 *
 * This module closes that gap. The decision is pure C (no ESP-IDF) so it is
 * host-tested (test/test_net_watchdog.c); main.c performs the recovery actions.
 *
 * Escalation (receiver only, and only when a home WLAN is configured):
 *   RECONNECT - the link has been down past down_timeout_ms: re-issue
 *               esp_wifi_connect(). The STA_DISCONNECTED callback makes exactly
 *               one attempt, and the driver never retries on its own, so a
 *               single failed reconnect leaves the device offline for good.
 *   REBOOT    - still down past reboot_timeout_ms after the reconnect: reboot to
 *               a known-good state (a wedged driver is the case a reconnect
 *               cannot fix).
 * A healthy sign at any time clears every stage, so a flapping link does not
 * reboot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NET_WD_OK = 0,
    NET_WD_RECONNECT, /* link down long enough: try to re-associate */
    NET_WD_REBOOT,    /* reconnect did not help: reboot */
} net_wd_action_t;

typedef struct {
    uint32_t down_timeout_ms;   /* silence before the first action */
    uint32_t reboot_timeout_ms; /* extra silence after the reconnect before reboot */
    uint32_t min_uptime_ms;     /* grace after boot before the link is judged */
} net_wd_policy_t;

typedef struct {
    net_wd_policy_t policy;
    uint64_t started_at_ms;
    uint64_t down_since_ms; /* 0 while the link is up */
    bool reconnect_tried;
    bool reboot_latched;
} net_wd_t;

/* Generous defaults: well above a normal router reboot, so a brief blip does
 * not trigger anything. main.c overrides them from Kconfig. */
void net_wd_default_policy(net_wd_policy_t *policy);

void net_wd_init(net_wd_t *w, const net_wd_policy_t *policy, uint64_t now_ms);

/* The link is healthy (associated and holding an IP). Clears every stage. */
void net_wd_note_up(net_wd_t *w, uint64_t now_ms);

/* Evaluate once. `up` is the current link state; returns the action to take. */
net_wd_action_t net_wd_tick(net_wd_t *w, uint64_t now_ms, bool up);
