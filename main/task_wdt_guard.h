/* Task-watchdog (TWDT) guard - the IDF glue around task_wdt_policy.c.
 *
 * The policy (which workers, how long they may block, what timeout covers them)
 * is pure C and host-tested in test_task_wdt_policy.c. This file only talks to
 * the ESP-IDF Task Watchdog: it configures the timer once (a generous timeout
 * with panic, so a stalled worker reboots instead of hanging), and the workers
 * subscribe themselves and feed it once per loop.
 *
 * A worker that stops making progress (deadlock on a mutex, a SPIFFS call that
 * never returns, a wedged driver) therefore trips the watchdog and reboots,
 * where before it would hang silently. A healthy but idle worker is safe: it
 * blocks with a bounded timeout and feeds the watchdog each loop, and the
 * timeout is chosen to cover the longest legitimate blocking call.
 *
 * Scope: the minute-history writer (both roles) and the sender's UART workers.
 * It complements liveness_guard.c (a global backstop fed by many places) and
 * web_guard.c (the dashboard) rather than replacing them.
 *
 * On the host build (no ESP-IDF) the guard is a set of inert stubs, so the
 * store and the tests compile without pulling in the RTOS watchdog.
 */
#ifndef TASK_WDT_GUARD_H
#define TASK_WDT_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "task_wdt_policy.h"

typedef struct {
    bool armed;         /* the TWDT is configured and running */
    bool panic;         /* a timeout triggers the panic handler (reboot) */
    uint32_t timeout_ms;/* configured timeout */
    uint32_t subscribed;/* how many workers have subscribed so far */
} task_wdt_guard_status_t;

#ifdef ESP_PLATFORM

/* Configure the TWDT once: init it if the IDF did not, otherwise reconfigure
 * the running one with our timeout/panic. Idempotent; call early in app_main,
 * before the workers start. Returns the ESP-IDF error (ESP_OK on success). */
esp_err_t task_wdt_guard_init(void);

/* Subscribe the calling task with the TWDT (idempotent). Returns true when the
 * task is subscribed afterwards. */
bool task_wdt_guard_subscribe(const char *worker_name);

/* Feed the watchdog on behalf of the calling task. A safe no-op when the task
 * is not subscribed. Returns true when a feed was accepted. */
bool task_wdt_guard_feed(void);

/* Snapshot for the console/diagnostics. */
void task_wdt_guard_get_status(task_wdt_guard_status_t *out);

#else /* host build: inert stubs so the store/tests link without the RTOS WDT */

#include <string.h>

static inline esp_err_t task_wdt_guard_init(void) { return ESP_OK; }
static inline bool task_wdt_guard_subscribe(const char *worker_name)
{
    (void)worker_name;
    return false;
}
static inline bool task_wdt_guard_feed(void) { return false; }
static inline void task_wdt_guard_get_status(task_wdt_guard_status_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

#endif /* ESP_PLATFORM */

#endif /* TASK_WDT_GUARD_H */
