/* Host test for the task-watchdog policy (pure C, no ESP-IDF).
 *
 * Covers the three decisions the firmware relies on:
 *   - which workers must be subscribed in which role (both roles keep the
 *     minute-history writer; only the sender has the UART workers),
 *   - the timeout arithmetic: the configured timeout must cover the longest
 *     legitimate blocking interval of every subscribed worker, with a margin,
 *     so a healthy but idle worker is never mistaken for a stalled one,
 *   - the stall predicate: a worker that has not fed for longer than the
 *     timeout is stalled, a worker that keeps feeding is not.
 *
 * The IDF glue (task_wdt_guard.c) is not part of this build; it only performs
 * the actions, so the decision is what is worth locking down here.
 */
#include <stdio.h>

#include "task_wdt_policy.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (!(cond)) {                                                      \
            ++failures;                                                     \
            printf("FAIL: %s\n", (msg));                                    \
        }                                                                   \
    } while (0)

int main(void)
{
    /* --- defaults ---------------------------------------------------------- */
    task_wdt_policy_t policy;
    task_wdt_policy_defaults(&policy);
    CHECK(policy.timeout_ms == 120000, "default timeout is 120 s");
    CHECK(policy.panic, "default triggers a panic (reboot)");
    CHECK(task_wdt_policy_valid(&policy), "default policy is valid");

    /* The defaults must cover both roles out of the box. */
    CHECK(task_wdt_policy_covers_role(&policy, TASK_WDT_ROLE_RECEIVER),
          "default timeout covers the receiver");
    CHECK(task_wdt_policy_covers_role(&policy, TASK_WDT_ROLE_SENDER),
          "default timeout covers the sender");

    /* --- worker table: which workers run in which role -------------------- */
    CHECK(task_wdt_worker_in_role(TASK_WDT_WORKER_POWER_HISTORY,
                                  TASK_WDT_ROLE_RECEIVER),
          "history writer runs on the receiver");
    CHECK(task_wdt_worker_in_role(TASK_WDT_WORKER_POWER_HISTORY,
                                  TASK_WDT_ROLE_SENDER),
          "history writer runs on the sender");
    CHECK(!task_wdt_worker_in_role(TASK_WDT_WORKER_UART_EVENT,
                                   TASK_WDT_ROLE_RECEIVER),
          "uart event worker is sender-only");
    CHECK(!task_wdt_worker_in_role(TASK_WDT_WORKER_UART_CAPTURE,
                                   TASK_WDT_ROLE_RECEIVER),
          "uart capture worker is sender-only");
    CHECK(task_wdt_worker_in_role(TASK_WDT_WORKER_UART_EVENT,
                                  TASK_WDT_ROLE_SENDER),
          "uart event worker runs on the sender");
    CHECK(task_wdt_worker_in_role(TASK_WDT_WORKER_UART_CAPTURE,
                                  TASK_WDT_ROLE_SENDER),
          "uart capture worker runs on the sender");

    /* Receiver subscribes 1 worker (history), sender 3. */
    CHECK(task_wdt_subscribed_count(TASK_WDT_ROLE_RECEIVER) == 1,
          "receiver subscribes exactly the history writer");
    CHECK(task_wdt_subscribed_count(TASK_WDT_ROLE_SENDER) == 3,
          "sender subscribes history + both uart workers");

    /* Names are stable strings (used in logs and the console). */
    CHECK(task_wdt_worker_name(TASK_WDT_WORKER_POWER_HISTORY)[0] == 'p',
          "history worker name");
    CHECK(task_wdt_worker_name(TASK_WDT_WORKER_UART_CAPTURE)[0] == 'u',
          "uart capture worker name");
    CHECK(task_wdt_worker_name(TASK_WDT_WORKER_COUNT) != NULL,
          "unknown worker name is still a string");

    /* --- timeout arithmetic ------------------------------------------------ */
    /* max_block * num/den + slack, clamped to [min, max]. */
    CHECK(task_wdt_required_timeout_ms(&policy, 60000) == 100000,
          "60 s block -> 3/2*60 s + 10 s = 100 s");
    CHECK(task_wdt_required_timeout_ms(&policy, 0) == policy.slack_ms,
          "a zero block needs only the slack");
    CHECK(task_wdt_required_timeout_ms(&policy, 1000) == 1500 + policy.slack_ms,
          "1 s block -> 3/2*1 s + 10 s = 11.5 s");
    CHECK(task_wdt_required_timeout_ms(&policy, 100000000u) == policy.max_timeout_ms,
          "an absurd block is capped at the maximum");

    /* The floor catches a policy whose slack is too small to cover a real
     * block, so the required timeout never falls below the configured minimum. */
    task_wdt_policy_t floor_policy = policy;
    floor_policy.slack_ms = 0;
    CHECK(task_wdt_required_timeout_ms(&floor_policy, 1000) == floor_policy.min_timeout_ms,
          "a small block is floored to the minimum");

    /* The required timeout always covers the worker's own block with margin. */
    for (int w = 0; w < (int)TASK_WDT_WORKER_COUNT; ++w) {
        uint32_t block = task_wdt_worker_max_block_ms((task_wdt_worker_t)w);
        uint32_t need = task_wdt_required_timeout_ms(&policy, block);
        CHECK(need >= block, "required timeout covers the worker's block");
        CHECK(need <= policy.timeout_ms, "the default timeout covers every worker");
    }

    /* --- a too-tight timeout must be rejected for the sender --------------- */
    task_wdt_policy_t tight = policy;
    tight.timeout_ms = 30000; /* < the 60 s history-batch wait * 3/2 */
    CHECK(!task_wdt_policy_covers_role(&tight, TASK_WDT_ROLE_SENDER),
          "a 30 s timeout does not cover the sender");
    CHECK(!task_wdt_policy_covers_role(&tight, TASK_WDT_ROLE_RECEIVER),
          "a 30 s timeout does not cover the receiver either");

    /* A timeout that exactly equals the required value is enough. */
    task_wdt_policy_t exact = policy;
    exact.timeout_ms = task_wdt_required_timeout_ms(&policy, 60000);
    CHECK(task_wdt_policy_covers_role(&exact, TASK_WDT_ROLE_SENDER),
          "the exact required timeout covers the sender");

    /* --- policy validity --------------------------------------------------- */
    task_wdt_policy_t bad = policy;
    bad.timeout_ms = 0;
    CHECK(!task_wdt_policy_valid(&bad), "a zero timeout is invalid");
    bad = policy;
    bad.margin_den = 0;
    CHECK(!task_wdt_policy_valid(&bad), "a zero margin denominator is invalid");
    bad = policy;
    bad.min_timeout_ms = 5000;
    bad.max_timeout_ms = 4000;
    CHECK(!task_wdt_policy_valid(&bad), "inverted bounds are invalid");
    bad = policy;
    bad.timeout_ms = 999999999u; /* above the max */
    CHECK(!task_wdt_policy_valid(&bad), "a timeout above the max is invalid");
    CHECK(!task_wdt_policy_valid(NULL), "a NULL policy is invalid");

    /* --- stall predicate --------------------------------------------------- */
    /* The timeout is exclusive: exactly at the timeout is not yet a stall. */
    CHECK(!task_wdt_is_stalled(1000, 1000, 120000), "just fed is not stalled");
    CHECK(!task_wdt_is_stalled(1000, 1000 + 120000, 120000),
          "at the timeout is not yet stalled");
    CHECK(task_wdt_is_stalled(1000, 1000 + 120001, 120000),
          "one ms past the timeout is stalled");
    CHECK(!task_wdt_is_stalled(5000, 1000, 120000),
          "a backwards clock is not a stall");
    CHECK(!task_wdt_is_stalled(1000, 1000, 0),
          "a zero timeout never stalls");

    /* A worker that keeps feeding inside the timeout never trips: simulate the
     * sender's history writer, which may block up to 60 s then feeds. */
    uint64_t last_feed = 0;
    for (uint64_t now = 0; now <= 3600000; now += 50000) {
        CHECK(!task_wdt_is_stalled(last_feed, now, policy.timeout_ms),
              "a worker feeding every 50 s stays alive");
        last_feed = now;
    }
    /* A worker that stops feeding is caught exactly one timeout later. */
    last_feed = 100000;
    CHECK(!task_wdt_is_stalled(last_feed, 100000 + policy.timeout_ms,
                               policy.timeout_ms),
          "a stalled worker is fine until the timeout elapses");
    CHECK(task_wdt_is_stalled(last_feed, 100000 + policy.timeout_ms + 1,
                              policy.timeout_ms),
          "a stalled worker is caught after the timeout");

    printf("task watchdog policy: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
