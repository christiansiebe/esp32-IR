/* Global liveness watchdog (both roles).
 *
 * The per-subsystem guards each watch one path: web_guard restarts the HTTP
 * server, link_fallback watches the ESP-NOW peer. None of them notices a
 * *global* stall - a worker that deadlocks on a mutex, a SPIFFS call that never
 * returns, an ISR storm - because the HTTP task may still be alive and the peer
 * simply goes quiet. The device then "refuses service" without a reset.
 *
 * This guard is the backstop: several independent places feed it (the UART
 * worker loop, every HTTP request, the web console). As long as *one* of them
 * still makes progress the timer keeps resetting; only a real freeze lets it
 * expire. It then escalates in stages:
 *
 *   WARN   - log a breadcrumb (persisted to the crash journal) once.
 *   PANIC  - a while later, log an ERROR (persisted) and reboot.
 *   REBOOT - if even that did not reset the device, reboot again as a last
 *            resort.
 *
 * The policy is pure C (no ESP-IDF) so it is host-tested; main.c performs the
 * feed and the actions.
 */
#ifndef LIVENESS_GUARD_H
#define LIVENESS_GUARD_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LIVENESS_OK = 0, /* nothing to do */
    LIVENESS_WARN,   /* silence exceeded feed_timeout: log + breadcrumb */
    LIVENESS_PANIC,  /* still silent: log ERROR + reboot (evidence kept) */
    LIVENESS_REBOOT, /* the panic did not reset the device: reboot again */
} liveness_action_t;

typedef struct {
    uint32_t feed_timeout_ms;   /* silence that counts as a hang */
    uint32_t panic_timeout_ms;  /* extra silence before the panic stage */
    uint32_t reboot_timeout_ms; /* extra silence after the panic before reboot */
    uint32_t min_uptime_ms;     /* grace after boot */
} liveness_policy_t;

typedef struct {
    liveness_policy_t policy;
    uint64_t started_at_ms;
    uint64_t last_feed_ms;
    uint64_t warned_at_ms;   /* 0 until the WARN stage fired */
    uint64_t panicked_at_ms; /* 0 until the PANIC stage fired */
} liveness_guard_t;

/* Generous defaults: a worker may block on SPIFFS for a while, so the first
 * stage is well above any legitimate stall, and the later stages only add to
 * it. main.c may tighten these at runtime. */
void liveness_default_policy(liveness_policy_t *policy);

void liveness_init(liveness_guard_t *guard, const liveness_policy_t *policy,
                   uint64_t now_ms);

/* Progress was observed (a worker pass, an HTTP request, a console command).
 * Clears every stage: the device is demonstrably alive again. */
void liveness_feed(liveness_guard_t *guard, uint64_t now_ms);

/* Milliseconds since the last feed (0 if the clock went backwards). */
uint32_t liveness_silence_ms(const liveness_guard_t *guard, uint64_t now_ms);

/* Advance the state machine and return the action to perform this tick.
 * Latching: WARN and PANIC fire once; REBOOT repeats until a feed clears it. */
liveness_action_t liveness_tick(liveness_guard_t *guard, uint64_t now_ms);

#endif /* LIVENESS_GUARD_H */
