/* Web-health guard policy: proves the escalation ladder and, above all, that
 * the guard stays passive until the self-probe has proven itself once. */
#include <stdio.h>

#include "web_guard.h"

static int failures;

static void check(const char *name, int condition)
{
    if (!condition) {
        printf("FAIL: %s\n", name);
        ++failures;
    } else {
        printf("ok: %s\n", name);
    }
}

static void test_inactive_until_probe_proven(void)
{
    web_guard_t g;
    web_guard_policy_t p;
    web_guard_default_policy(&p);
    web_guard_init(&g, &p, 0);

    /* Never probed: even after a long silence the guard must not act. */
    check("passive before any successful probe",
          web_guard_tick(&g, 1000000) == WEB_GUARD_OK);
    check("is_active false before probe", !web_guard_is_active(&g));
}

static void test_healthy_traffic_stays_ok(void)
{
    web_guard_t g;
    web_guard_policy_t p;
    web_guard_default_policy(&p);
    web_guard_init(&g, &p, 0);
    web_guard_note_probe(&g, 1000, true);

    check("active after a good probe", web_guard_is_active(&g));
    /* Real requests keep it healthy past the uptime grace. */
    web_guard_note_request(&g, 50000);
    check("ok while traffic flows",
          web_guard_tick(&g, 60000) == WEB_GUARD_OK);
}

static void test_restart_then_reboot(void)
{
    web_guard_t g;
    web_guard_policy_t p;
    web_guard_default_policy(&p); /* threshold 30s, reboot 90s, grace 45s */
    web_guard_init(&g, &p, 0);
    web_guard_note_probe(&g, 1000, true);

    /* Within the uptime grace: silent but not acted on yet. */
    check("no action inside the uptime grace",
          web_guard_tick(&g, 40000) == WEB_GUARD_OK);

    /* Silence exceeds the threshold -> restart. */
    check("restart when the threshold is exceeded",
          web_guard_tick(&g, 100000) == WEB_GUARD_RESTART);

    /* A failed restart keeps the guard trying after the retry timeout. */
    web_guard_note_restart_attempt(&g, 100000, false);
    check("no immediate second attempt",
          web_guard_tick(&g, 110000) == WEB_GUARD_OK);
    check("restart retried after the retry timeout",
          web_guard_tick(&g, 120000) == WEB_GUARD_RESTART);

    /* Dead for longer than the reboot window -> escalate to reboot. */
    web_guard_note_restart_attempt(&g, 120000, false);
    check("reboot after the reboot window",
          web_guard_tick(&g, 195000) == WEB_GUARD_REBOOT);
}

static void test_successful_restart_resets(void)
{
    web_guard_t g;
    web_guard_policy_t p;
    web_guard_default_policy(&p);
    web_guard_init(&g, &p, 0);
    web_guard_note_probe(&g, 1000, true);

    check("restart due", web_guard_tick(&g, 100000) == WEB_GUARD_RESTART);
    web_guard_note_restart_attempt(&g, 100000, true);
    check("restart counted", g.restarts == 1);
    check("healthy again after a good restart",
          web_guard_tick(&g, 110000) == WEB_GUARD_OK);
}

/* The idle-restart bug: the probe's /healthz handler stamps last_request_ms a
 * few ms *after* the tick captured now_ms, so `last` > `now`. On uint64_t the
 * subtraction `now_ms - last` then underflows to ~2^64 and reads as an endless
 * silence, so the guard restarted the (healthy) server roughly every
 * restart_timeout_ms while idle — the "restarts climbs ~1.2/min, last activity
 * stays ~1 s" report. A saturating subtraction must treat it as healthy. */
static void test_activity_stamp_ahead_is_not_silence(void)
{
    web_guard_t g;
    web_guard_policy_t p;
    web_guard_default_policy(&p); /* threshold 30s, restart 15s, grace 45s */
    web_guard_init(&g, &p, 0);
    web_guard_note_probe(&g, 1000, true);

    /* now_ms captured at 100000; the /healthz handler stamped 100010 (10 ms
     * later). Without the clamp this tick returned WEB_GUARD_RESTART. */
    web_guard_note_request(&g, 100010);
    check("a few ms in the future is not an endless silence",
          web_guard_tick(&g, 100000) == WEB_GUARD_OK);
    check("no restart when the activity stamp is ahead", g.restarts == 0);

    /* Repeat the probe/tick pattern as the idle loop would: every 5 s the tick
     * captures now_ms and the handler stamps a few ms later. None of these may
     * restart, and the activity stays fresh. */
    uint64_t now = 105000;
    for (int i = 0; i < 40; ++i) {
        web_guard_note_request(&g, now + 3); /* handler stamps 3 ms after now */
        web_guard_note_probe(&g, now, true);
        check("idle probe loop stays healthy",
              web_guard_tick(&g, now) == WEB_GUARD_OK);
        now += 5000;
    }
    check("idle probe loop never restarts the server", g.restarts == 0);
}

int main(void)
{
    test_inactive_until_probe_proven();
    test_healthy_traffic_stays_ok();
    test_restart_then_reboot();
    test_successful_restart_resets();
    test_activity_stamp_ahead_is_not_silence();

    if (failures != 0) {
        printf("web_guard: %d failure(s)\n", failures);
        return 1;
    }
    printf("web_guard: all checks passed\n");
    return 0;
}
