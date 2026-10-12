#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pure, host-testable health/watchdog policy for the receiver.
 *
 * The receiver's failure mode is not a crash but a silent hang: the SML data
 * stops arriving (sender or ESP-NOW lost) while the device keeps serving a
 * stale dashboard. A periodic task evaluates these rules and reboots when the
 * condition is reached. Keeping the decision pure makes the thresholds and the
 * latch behaviour unit-testable without a device.
 *
 * Time base is milliseconds since boot; the caller feeds a monotonic clock.
 */

typedef enum {
    HEALTH_OK = 0,
    /* No SML value received for longer than the stale limit. */
    HEALTH_STALE_DATA = 1,
    /* The FreeRTOS heap fell below the low-water threshold. */
    HEALTH_LOW_HEAP = 2,
} health_reason_t;

typedef struct {
    uint32_t stale_data_ms;   /* 0 disables the stale-data rule */
    uint32_t low_heap_bytes;  /* 0 disables the low-heap rule */
    uint32_t grace_ms;        /* uptime before any rule may fire */
} health_policy_t;

typedef struct {
    bool latched;
    health_reason_t reason;
} health_state_t;

/* A practical default for a mains-powered receiver. */
void health_policy_defaults(health_policy_t *policy);

void health_state_reset(health_state_t *state);

/* Evaluate the rules once. `uptime_ms` is the time since boot, `last_rx_ms`
 * the time the last SML value arrived (0 = never), `free_heap` the current
 * free heap. Returns the reason to act on, or HEALTH_OK. Once a reason has
 * fired it stays latched (a reboot takes time; we must not flap). */
health_reason_t health_evaluate(const health_policy_t *policy,
                                health_state_t *state, uint32_t uptime_ms,
                                uint32_t last_rx_ms, uint32_t free_heap);

const char *health_reason_name(health_reason_t reason);
