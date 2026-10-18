/* HTTP-server health guard (receiver).
 *
 * The post-OTA rollback (boot_confirm.c) only covers the boot window: an image
 * that boots and passes the stability window is confirmed, and from then on
 * nobody checks whether the dashboard is still answering. If the httpd task
 * dies or hangs later, the web UI stays gone with no recovery - exactly the
 * "web offline for minutes, no rollback" report.
 *
 * This guard closes that gap: a periodic self-probe keeps the server honest,
 * and when it goes silent the server is restarted, and as a last resort the
 * device reboots (which re-enters the boot_confirm policy).
 *
 * Safety: the guard only arms itself once the self-probe has actually worked
 * once (probe_works). A build without the loopback path therefore stays
 * passive instead of rebooting a healthy device. The policy/tick logic is pure
 * (no ESP-IDF) so it is host-tested; the probe/restart glue lives at the bottom
 * of web_guard.c behind ESP_PLATFORM.
 */
#ifndef WEB_GUARD_H
#define WEB_GUARD_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WEB_GUARD_OK = 0, /* nothing to do */
    WEB_GUARD_RESTART, /* server silent: stop + start httpd */
    WEB_GUARD_REBOOT,  /* restart did not help: reboot the device */
} web_guard_action_t;

typedef struct {
    uint32_t threshold_ms;       /* silence that counts as dead */
    uint32_t restart_timeout_ms; /* wait between restart attempts */
    uint32_t reboot_timeout_ms;  /* dead for this long -> reboot */
    uint32_t min_uptime_ms;      /* grace after boot / restart */
} web_guard_policy_t;

typedef struct {
    web_guard_policy_t policy;
    uint64_t started_at_ms;
    uint64_t last_request_ms; /* last served HTTP request (real) */
    uint64_t last_probe_ms;   /* last successful self-probe */
    uint64_t dead_since_ms;   /* when the silence was first seen (log only) */
    uint64_t first_attempt_ms;/* first restart attempt of this outage */
    uint64_t last_attempt_ms;
    uint32_t restarts;        /* successful restarts */
    bool probe_works; /* self-probe path proven -> guard may act */
    bool dead;
} web_guard_t;

void web_guard_default_policy(web_guard_policy_t *policy);
void web_guard_init(web_guard_t *guard, const web_guard_policy_t *policy,
                    uint64_t now_ms);

/* A real HTTP request was served. */
void web_guard_note_request(web_guard_t *guard, uint64_t now_ms);
/* Result of one self-probe. ok=false is what eventually trips the guard. */
void web_guard_note_probe(web_guard_t *guard, uint64_t now_ms, bool ok);
/* Result of a stop+start cycle (ok=false keeps the guard trying). */
void web_guard_note_restart_attempt(web_guard_t *guard, uint64_t now_ms, bool ok);

web_guard_action_t web_guard_tick(web_guard_t *guard, uint64_t now_ms);

bool web_guard_is_active(const web_guard_t *guard);
uint64_t web_guard_last_activity_ms(const web_guard_t *guard);

#ifdef ESP_PLATFORM
#include "esp_http_server.h"
/* Start the guard task. server/config are the live httpd handle and its
 * config; the guard restarts with the same config and re-registers handlers. */
void web_guard_start(httpd_handle_t *server, const httpd_config_t *config,
                     void (*register_handlers)(httpd_handle_t));
/* Register the /healthz probe endpoint (no auth, no state). */
esp_err_t web_guard_register_healthz(httpd_handle_t server);
/* Runtime state for the console/diagnostics. */
bool web_guard_active(void);
uint32_t web_guard_restart_count(void);
uint32_t web_guard_last_activity_age_ms(void);
/* Last self-probe result: transport error, HTTP status, cumulative failures. */
void web_guard_probe_stats(uint32_t *err, int *status, uint32_t *fails);
#endif

#endif /* WEB_GUARD_H */
