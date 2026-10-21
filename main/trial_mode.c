#include "trial_mode.h"

#include <stddef.h>

void trial_policy_defaults(trial_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }
    policy->window_ms = 60000;
    policy->grace_ms = 10000;
}

void trial_begin(trial_t *t, const trial_policy_t *policy, uint64_t now_ms)
{
    if (t == NULL) {
        return;
    }
    t->policy = policy != NULL ? *policy : (trial_policy_t){0};
    if (policy == NULL) {
        trial_policy_defaults(&t->policy);
    }
    t->started_at_ms = now_ms;
    t->pending = true;
    t->saw_healthy = false;
    t->decided = false;
}

bool trial_boot_should_revert(bool pending_at_boot)
{
    /* The marker only survives a reset that happened while the trial was still
     * running. A completed trial clears it before the device is ever reset on
     * purpose, so a pending marker at boot always means "the trial did not
     * finish -> drop it". */
    return pending_at_boot;
}

trial_action_t trial_tick(trial_t *t, uint64_t now_ms, bool healthy)
{
    if (t == NULL || !t->pending) {
        return TRIAL_ACT_NONE;
    }
    if (t->decided) {
        return TRIAL_ACT_NONE;
    }
    /* Grace: association/DHCP are still settling, so a "not healthy yet" must
     * not be read as a broken setting. */
    if (now_ms - t->started_at_ms < t->policy.grace_ms) {
        return TRIAL_ACT_NONE;
    }
    if (!healthy) {
        /* Fail fast: one unhealthy sample past the grace reverts right away.
         * The caller reboots; the next boot drops the candidate. */
        t->decided = true;
        return TRIAL_ACT_REVERT;
    }
    t->saw_healthy = true;
    if (now_ms - t->started_at_ms >= t->policy.window_ms) {
        t->decided = true;
        return TRIAL_ACT_COMMIT;
    }
    return TRIAL_ACT_NONE;
}
