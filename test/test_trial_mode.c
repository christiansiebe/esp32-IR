/* Host test for the trial-mode auto-revert policy (trial_mode.c).
 *
 * No mocks: the real module is compiled in and driven with a virtual clock and
 * a virtual health signal, exactly as main.c would drive it. */
#include <assert.h>
#include <stdio.h>

#include "trial_mode.h"

static int g_checks;

static void check(bool cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        printf("FAIL: %s\n", what);
        assert(0);
    }
}

static trial_policy_t policy(void)
{
    trial_policy_t p;
    trial_policy_defaults(&p);
    p.window_ms = 60000;
    p.grace_ms = 10000;
    return p;
}

/* A trial that stays healthy for the whole window commits exactly once. */
static void test_commit(void)
{
    trial_t t;
    trial_policy_t p = policy();
    trial_begin(&t, &p, 1000);
    check(t.pending, "trial begins pending");
    for (uint64_t now = 1000; now < 1000 + p.window_ms; now += 5000) {
        check(trial_tick(&t, now, true) == TRIAL_ACT_NONE, "healthy before window -> none");
    }
    check(trial_tick(&t, 1000 + p.window_ms, true) == TRIAL_ACT_COMMIT,
          "healthy at window -> commit");
    /* Final: no second decision. */
    check(trial_tick(&t, 1000 + p.window_ms + 1000, true) == TRIAL_ACT_NONE,
          "commit is final");
}

/* A health failure past the grace reverts immediately (the caller reboots). */
static void test_revert_immediate(void)
{
    trial_t t;
    trial_policy_t p = policy();
    trial_begin(&t, &p, 1000);
    /* Still inside the grace: an unhealthy sample is ignored. */
    check(trial_tick(&t, 1000 + p.grace_ms - 1, false) == TRIAL_ACT_NONE,
          "unhealthy inside grace -> none");
    /* Past the grace: one unhealthy sample reverts right away. */
    check(trial_tick(&t, 1000 + p.grace_ms, false) == TRIAL_ACT_REVERT,
          "unhealthy past grace -> revert");
    check(trial_tick(&t, 1000 + p.grace_ms + 1000, true) == TRIAL_ACT_NONE,
          "revert is final");
}

/* A trial that never becomes healthy must not commit at the deadline. */
static void test_never_healthy(void)
{
    trial_t t;
    trial_policy_t p = policy();
    trial_begin(&t, &p, 0);
    /* Unhealthy past grace reverts before the window can elapse. */
    check(trial_tick(&t, p.grace_ms, false) == TRIAL_ACT_REVERT,
          "never healthy -> revert at grace");
}

/* A short unhealthy blip inside the grace does not kill a good trial. */
static void test_blip_inside_grace(void)
{
    trial_t t;
    trial_policy_t p = policy();
    trial_begin(&t, &p, 0);
    check(trial_tick(&t, p.grace_ms - 1, false) == TRIAL_ACT_NONE, "blip in grace ok");
    check(trial_tick(&t, p.grace_ms, true) == TRIAL_ACT_NONE, "healthy after blip ok");
    check(trial_tick(&t, p.window_ms, true) == TRIAL_ACT_COMMIT, "still commits");
}

/* A tick without an active trial does nothing. */
static void test_inactive(void)
{
    trial_t t = {0};
    check(trial_tick(&t, 100000, true) == TRIAL_ACT_NONE, "no trial -> none");
    check(trial_tick(&t, 100000, false) == TRIAL_ACT_NONE, "no trial unhealthy -> none");
    check(trial_tick(NULL, 0, true) == TRIAL_ACT_NONE, "NULL is safe");
}

/* The boot decision is the whole rollback: a pending marker always reverts. */
static void test_boot_decision(void)
{
    check(trial_boot_should_revert(true), "pending at boot -> revert");
    check(!trial_boot_should_revert(false), "no marker -> keep committed");
}

/* Defaults are sane and the begin call resets a previously decided trial. */
static void test_begin_resets(void)
{
    trial_t t;
    trial_policy_t p = policy();
    trial_begin(&t, &p, 0);
    check(trial_tick(&t, p.window_ms, true) == TRIAL_ACT_COMMIT, "first trial commits");
    trial_begin(&t, &p, 100000);
    check(t.pending && !t.decided, "begin clears the decided latch");
    check(trial_tick(&t, 100000 + p.grace_ms, false) == TRIAL_ACT_REVERT,
          "a fresh trial can still revert");
}

int main(void)
{
    test_commit();
    test_revert_immediate();
    test_never_healthy();
    test_blip_inside_grace();
    test_inactive();
    test_boot_decision();
    test_begin_resets();
    printf("trial_mode: %d checks passed\n", g_checks);
    return 0;
}
