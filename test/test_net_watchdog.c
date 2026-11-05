/* Host test for the receiver Wi-Fi link watchdog policy (net_watchdog.c).
 *
 * No mocks: the real module is compiled in and driven with a virtual clock and
 * a virtual link state, exactly like the guard would see on the device. */
#include <assert.h>
#include <stdio.h>

#include "net_watchdog.h"

static int g_checks;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        printf("FAIL: %s\n", what);
        assert(0);
    }
}

static net_wd_policy_t policy(void)
{
    net_wd_policy_t p;
    net_wd_default_policy(&p);
    p.down_timeout_ms = 1000;
    p.reboot_timeout_ms = 2000;
    p.min_uptime_ms = 500;
    return p;
}

/* A healthy link never acts. */
static void test_healthy_link(void)
{
    net_wd_t w;
    net_wd_init(&w, &(net_wd_policy_t){0}, 0);
    net_wd_policy_t p = policy();
    net_wd_init(&w, &p, 0);
    for (uint64_t t = 0; t <= 100000; t += 500) {
        check(net_wd_tick(&w, t, true) == NET_WD_OK, "healthy link stays OK");
    }
}

/* A short blip inside the down timeout is not an outage. */
static void test_blip_ignored(void)
{
    net_wd_t w;
    net_wd_policy_t p = policy();
    net_wd_init(&w, &p, 0);
    check(net_wd_tick(&w, 600, false) == NET_WD_OK, "down inside timeout -> OK");
    check(net_wd_tick(&w, 900, false) == NET_WD_OK, "still inside timeout -> OK");
    /* Recovered before the timeout: no action, state cleared. */
    check(net_wd_tick(&w, 1000, true) == NET_WD_OK, "recovered -> OK");
    check(w.down_since_ms == 0, "recovery clears the down edge");
    /* A later blip starts a fresh clock (does not inherit the old edge). */
    check(net_wd_tick(&w, 1500, false) == NET_WD_OK, "new blip -> OK");
    check(net_wd_tick(&w, 2600, false) == NET_WD_RECONNECT, "new blip escalates");
}

/* Boot grace: a link that is down at boot is not judged immediately. */
static void test_boot_grace(void)
{
    net_wd_t w;
    net_wd_policy_t p = policy();
    net_wd_init(&w, &p, 0);
    check(net_wd_tick(&w, 100, false) == NET_WD_OK, "within grace -> OK");
    check(net_wd_tick(&w, 400, false) == NET_WD_OK, "still within grace -> OK");
    /* Grace ends at 500; the down edge starts there. */
    check(net_wd_tick(&w, 600, false) == NET_WD_OK, "edge just set -> OK");
    check(net_wd_tick(&w, 1500, false) == NET_WD_OK, "short of down timeout -> OK");
    check(net_wd_tick(&w, 1600, false) == NET_WD_RECONNECT, "reconnect after grace+timeout");
}

/* The full escalation: down -> reconnect -> reboot. */
static void test_escalation(void)
{
    net_wd_t w;
    net_wd_policy_t p = policy();
    net_wd_init(&w, &p, 0);
    /* Grace passes at 500, down edge set at 600. */
    check(net_wd_tick(&w, 600, false) == NET_WD_OK, "down edge set");
    check(net_wd_tick(&w, 1000, false) == NET_WD_OK, "before reconnect timeout");
    check(net_wd_tick(&w, 1600, false) == NET_WD_RECONNECT, "first action = reconnect");
    /* The reconnect is tried once; a second tick must not reconnect again. */
    check(net_wd_tick(&w, 1700, false) == NET_WD_OK, "no repeat reconnect");
    check(net_wd_tick(&w, 3000, false) == NET_WD_OK, "before reboot timeout");
    check(net_wd_tick(&w, 3600, false) == NET_WD_REBOOT, "reboot after reconnect window");
    /* Sticky: a later "up" must not cancel a reboot already decided. */
    check(net_wd_tick(&w, 3700, true) == NET_WD_REBOOT, "reboot latched");
    check(net_wd_tick(&w, 100000, true) == NET_WD_REBOOT, "reboot stays latched");
}

/* A reconnect that works clears the escalation entirely. */
static void test_reconnect_recovers(void)
{
    net_wd_t w;
    net_wd_policy_t p = policy();
    net_wd_init(&w, &p, 0);
    check(net_wd_tick(&w, 600, false) == NET_WD_OK, "down edge set");
    check(net_wd_tick(&w, 1600, false) == NET_WD_RECONNECT, "reconnect");
    check(net_wd_tick(&w, 2000, true) == NET_WD_OK, "reconnect worked -> OK");
    check(w.reconnect_tried == false && w.reboot_latched == false,
          "recovery resets both stages");
    /* And the next outage escalates from scratch. */
    check(net_wd_tick(&w, 3000, false) == NET_WD_OK, "new outage edge");
    check(net_wd_tick(&w, 4000, false) == NET_WD_RECONNECT, "escalates again");
}

/* A zero/low policy must not fire on the same tick the edge is set. */
static void test_boundary(void)
{
    net_wd_t w;
    net_wd_policy_t p = policy();
    net_wd_init(&w, &p, 0);
    check(net_wd_tick(&w, 500, false) == NET_WD_OK, "edge at exactly grace end");
    check(net_wd_tick(&w, 1499, false) == NET_WD_OK, "just below the timeout");
    check(net_wd_tick(&w, 1500, false) == NET_WD_RECONNECT, "exactly at the timeout");
}

int main(void)
{
    test_healthy_link();
    test_blip_ignored();
    test_boot_grace();
    test_escalation();
    test_reconnect_recovers();
    test_boundary();
    printf("net watchdog: %d checks, 0 failures\n", g_checks);
    return 0;
}
