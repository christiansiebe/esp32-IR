#include "task_wdt_policy.h"

#include <stddef.h>

void task_wdt_policy_defaults(task_wdt_policy_t *policy)
{
    /* 120 s: twice the longest legitimate blocking interval (the 60 s history
     * batch wait) plus headroom, so an idle worker never looks stalled while a
     * truly wedged one still reboots in a bounded time. */
    policy->timeout_ms = 120000;
    policy->min_timeout_ms = 5000;
    policy->max_timeout_ms = 3600000; /* 1 h cap: a bogus block must not disable it */
    policy->margin_num = 3;
    policy->margin_den = 2;
    policy->slack_ms = 10000;
    policy->panic = true;
}

const char *task_wdt_worker_name(task_wdt_worker_t worker)
{
    switch (worker) {
    case TASK_WDT_WORKER_POWER_HISTORY: return "power_history";
    case TASK_WDT_WORKER_UART_EVENT:    return "uart_events";
    case TASK_WDT_WORKER_UART_CAPTURE:  return "uart_capture";
    case TASK_WDT_WORKER_COUNT:
    default:                            return "unknown";
    }
}

bool task_wdt_worker_in_role(task_wdt_worker_t worker, task_wdt_role_t role)
{
    switch (worker) {
    case TASK_WDT_WORKER_POWER_HISTORY:
        /* The minute-history writer runs on both roles (the sender records what
         * it decodes off the UART, the receiver what it receives). */
        return true;
    case TASK_WDT_WORKER_UART_EVENT:
    case TASK_WDT_WORKER_UART_CAPTURE:
        /* The UART workers only exist in the sender. */
        return role == TASK_WDT_ROLE_SENDER;
    case TASK_WDT_WORKER_COUNT:
    default:
        return false;
    }
}

uint32_t task_wdt_worker_max_block_ms(task_wdt_worker_t worker)
{
    switch (worker) {
    case TASK_WDT_WORKER_POWER_HISTORY:
        /* The worker drains the queue in batches; after the first record it may
         * wait up to POWER_HISTORY_BATCH_TIMEOUT_MS (60 s) for more before it
         * writes. It feeds the watchdog around that wait, so the wait is the
         * longest interval it can legitimately go without feeding. */
        return 60000;
    case TASK_WDT_WORKER_UART_EVENT:
        /* Blocks on the event queue with a 1 s timeout. */
        return 1000;
    case TASK_WDT_WORKER_UART_CAPTURE:
        /* UART read timeout (50 ms) plus the longest adaptive light-sleep window
         * the controller may request (1500 ms). */
        return 2000;
    case TASK_WDT_WORKER_COUNT:
    default:
        return 0;
    }
}

uint32_t task_wdt_required_timeout_ms(const task_wdt_policy_t *policy,
                                      uint32_t max_block_ms)
{
    if (policy == NULL || policy->margin_den == 0) {
        return policy != NULL ? policy->timeout_ms : 0;
    }
    uint64_t required = (uint64_t)max_block_ms * policy->margin_num /
                        policy->margin_den;
    required += policy->slack_ms;
    if (required < policy->min_timeout_ms) {
        return policy->min_timeout_ms;
    }
    if (required > policy->max_timeout_ms) {
        return policy->max_timeout_ms;
    }
    return (uint32_t)required;
}

bool task_wdt_policy_covers_role(const task_wdt_policy_t *policy,
                                 task_wdt_role_t role)
{
    if (policy == NULL) {
        return false;
    }
    for (int w = 0; w < (int)TASK_WDT_WORKER_COUNT; ++w) {
        task_wdt_worker_t worker = (task_wdt_worker_t)w;
        if (!task_wdt_worker_in_role(worker, role)) {
            continue;
        }
        uint32_t required = task_wdt_required_timeout_ms(
            policy, task_wdt_worker_max_block_ms(worker));
        if (policy->timeout_ms < required) {
            return false;
        }
    }
    return true;
}

bool task_wdt_policy_valid(const task_wdt_policy_t *policy)
{
    if (policy == NULL) {
        return false;
    }
    if (policy->timeout_ms == 0 || policy->min_timeout_ms == 0 ||
        policy->max_timeout_ms == 0) {
        return false;
    }
    if (policy->min_timeout_ms > policy->max_timeout_ms) {
        return false;
    }
    if (policy->timeout_ms < policy->min_timeout_ms ||
        policy->timeout_ms > policy->max_timeout_ms) {
        return false;
    }
    if (policy->margin_den == 0 || policy->margin_num == 0) {
        return false;
    }
    return true;
}

bool task_wdt_is_stalled(uint64_t last_feed_ms, uint64_t now_ms,
                         uint32_t timeout_ms)
{
    if (now_ms <= last_feed_ms) {
        return false; /* a backwards/zero delta is not a stall */
    }
    return (now_ms - last_feed_ms) > (uint64_t)timeout_ms;
}

unsigned task_wdt_subscribed_count(task_wdt_role_t role)
{
    unsigned count = 0;
    for (int w = 0; w < (int)TASK_WDT_WORKER_COUNT; ++w) {
        if (task_wdt_worker_in_role((task_wdt_worker_t)w, role)) {
            ++count;
        }
    }
    return count;
}
