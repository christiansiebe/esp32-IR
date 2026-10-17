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

int main(void)
{
    test_inactive_until_probe_proven();
    test_healthy_traffic_stays_ok();
    test_restart_then_reboot();
    test_successful_restart_resets();

    if (failures != 0) {
        printf("web_guard: %d failure(s)\n", failures);
        return 1;
    }
    printf("web_guard: all checks passed\n");
    return 0;
}
