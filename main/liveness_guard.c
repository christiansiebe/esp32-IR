#include "liveness_guard.h"

void liveness_default_policy(liveness_policy_t *policy)
{
    policy->feed_timeout_ms = 60000;
    policy->panic_timeout_ms = 30000;
    policy->reboot_timeout_ms = 30000;
    policy->min_uptime_ms = 90000;
}

void liveness_init(liveness_guard_t *guard, const liveness_policy_t *policy,
                   uint64_t now_ms)
{
    guard->policy = *policy;
    guard->started_at_ms = now_ms;
    guard->last_feed_ms = now_ms;
    guard->warned_at_ms = 0;
    guard->panicked_at_ms = 0;
}

void liveness_feed(liveness_guard_t *guard, uint64_t now_ms)
{
    guard->last_feed_ms = now_ms;
    guard->warned_at_ms = 0;
    guard->panicked_at_ms = 0;
}

uint32_t liveness_silence_ms(const liveness_guard_t *guard, uint64_t now_ms)
{
    if (now_ms <= guard->last_feed_ms) {
        return 0;
    }
    uint64_t silence = now_ms - guard->last_feed_ms;
    if (silence > 0xFFFFFFFFu) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)silence;
}

liveness_action_t liveness_tick(liveness_guard_t *guard, uint64_t now_ms)
{
    if (now_ms < guard->started_at_ms ||
        now_ms - guard->started_at_ms < guard->policy.min_uptime_ms) {
        return LIVENESS_OK;
    }
    uint32_t silence = liveness_silence_ms(guard, now_ms);
    if (silence < guard->policy.feed_timeout_ms) {
        return LIVENESS_OK;
    }

    uint32_t warn_limit = guard->policy.feed_timeout_ms;
    uint32_t panic_limit = warn_limit + guard->policy.panic_timeout_ms;
    uint32_t reboot_limit = panic_limit + guard->policy.reboot_timeout_ms;

    if (silence >= reboot_limit) {
        return LIVENESS_REBOOT;
    }
    if (silence >= panic_limit) {
        if (guard->panicked_at_ms == 0) {
            guard->panicked_at_ms = now_ms;
            return LIVENESS_PANIC;
        }
        return LIVENESS_OK;
    }
    /* silence >= feed_timeout: the first stage. */
    if (guard->warned_at_ms == 0) {
        guard->warned_at_ms = now_ms;
        return LIVENESS_WARN;
    }
    return LIVENESS_OK;
}
