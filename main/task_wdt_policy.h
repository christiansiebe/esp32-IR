/* Task-watchdog (TWDT) policy - the pure decision logic.
 *
 * The long-running workers (the minute-history writer and the UART workers)
 * block on a queue/semaphore/UART read, so they are the tasks a wedged driver
 * or a deadlock would silently park. They are registered with the ESP-IDF Task
 * Watchdog Timer: each subscribes itself and feeds the watchdog once per loop,
 * and a worker that stops making progress trips the watchdog (panic -> reboot)
 * instead of hanging forever.
 *
 * This module holds only the policy: which workers run in which role, the
 * longest interval a worker may legitimately spend inside one blocking call,
 * and the timeout arithmetic that must cover it. It is pure C (no ESP-IDF), so
 * the "a healthy worker must not trip, a stalled one must" decision is
 * host-tested (test_task_wdt_policy.c). task_wdt_guard.c is the IDF glue.
 */
#ifndef TASK_WDT_POLICY_H
#define TASK_WDT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Stable worker names. Macros (not just the task_wdt_worker_name() function) so
 * power_history.c can pass the name without linking task_wdt_policy.c on the
 * host, where only the pure guard stubs are available. */
#define TASK_WDT_NAME_POWER_HISTORY "power_history"
#define TASK_WDT_NAME_UART_EVENT    "uart_events"
#define TASK_WDT_NAME_UART_CAPTURE  "uart_capture"
#define TASK_WDT_NAME_HEALTH        "health"

/* Long-running workers that must prove liveness to the TWDT. */
typedef enum {
    TASK_WDT_WORKER_POWER_HISTORY = 0, /* drains the minute-history queue (both roles) */
    TASK_WDT_WORKER_UART_EVENT,        /* UART error/statistics worker (sender) */
    TASK_WDT_WORKER_UART_CAPTURE,      /* UART byte reader / SML decoder (sender) */
    TASK_WDT_WORKER_HEALTH,            /* receiver self-heal watchdog (receiver) */
    TASK_WDT_WORKER_COUNT
} task_wdt_worker_t;

/* Firmware role a worker runs in. Mirrors CONFIG_ESPNOW_ROLE_*. */
typedef enum {
    TASK_WDT_ROLE_RECEIVER = 0,
    TASK_WDT_ROLE_SENDER,
} task_wdt_role_t;

typedef struct {
    /* Configured TWDT timeout. Generous on purpose: it must exceed the longest
     * legitimate blocking interval of any subscribed worker, so an idle (but
     * healthy) worker is never mistaken for a stalled one. */
    uint32_t timeout_ms;
    uint32_t min_timeout_ms; /* floor for the required-timeout math */
    uint32_t max_timeout_ms; /* ceiling for the required-timeout math */
    uint32_t margin_num;     /* required = max_block * num/den + slack */
    uint32_t margin_den;
    uint32_t slack_ms;
    bool panic; /* trigger the panic handler (reboot) on a timeout */
} task_wdt_policy_t;

/* Defaults: 120 s timeout (covers the 60 s history-batch wait with a 2x
 * margin), panic on. Matches the value proposed in TODO.md. */
void task_wdt_policy_defaults(task_wdt_policy_t *policy);

/* Stable name of a worker, for logs and the console. */
const char *task_wdt_worker_name(task_wdt_worker_t worker);

/* Does this worker run in the given role (and must therefore be subscribed)? */
bool task_wdt_worker_in_role(task_wdt_worker_t worker, task_wdt_role_t role);

/* The longest interval a worker may legitimately spend inside a single blocking
 * call (queue wait / UART read / batch flush) before it is expected to feed the
 * watchdog again. */
uint32_t task_wdt_worker_max_block_ms(task_wdt_worker_t worker);

/* Smallest timeout that safely covers a blocking interval of max_block_ms,
 * clamped to [min_timeout_ms, max_timeout_ms]. */
uint32_t task_wdt_required_timeout_ms(const task_wdt_policy_t *policy,
                                      uint32_t max_block_ms);

/* True when the configured timeout covers every worker subscribed in role. */
bool task_wdt_policy_covers_role(const task_wdt_policy_t *policy,
                                 task_wdt_role_t role);

/* True when the policy is internally consistent (ordered bounds, non-zero
 * margin denominator, non-zero timeout). */
bool task_wdt_policy_valid(const task_wdt_policy_t *policy);

/* Stall predicate mirroring what the hardware enforces: a worker that has not
 * fed for timeout_ms is stalled. A backwards clock (now < last_feed) is not a
 * stall. */
bool task_wdt_is_stalled(uint64_t last_feed_ms, uint64_t now_ms,
                         uint32_t timeout_ms);

/* Number of workers that must be subscribed for a role. */
unsigned task_wdt_subscribed_count(task_wdt_role_t role);

#endif /* TASK_WDT_POLICY_H */
