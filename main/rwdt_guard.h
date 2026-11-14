/* RTC/LP watchdog (RWDT) backstop - the IDF glue around rwdt_policy.c.
 *
 * The RWDT is a hardware watchdog clocked by the RTC slow clock. Its feed is a
 * single register write (lpwdt_ll_feed: hw->feed.rtc_wdt_feed = 1) with no
 * flash or cache access, and on a stage-0 timeout it resets the whole system
 * in hardware. That makes it the only watchdog that still fires when a
 * SPIFFS/flash operation stalls the CPU with the cache disabled - the exact
 * "hard hang, no reboot" failure this project saw on the C6.
 *
 * Feeding policy (gated feed): the guard is fed ONLY on observed forward
 * progress, from liveness_note_alive() (the aggregate fed by the UART worker
 * and every HTTP request). If progress stops, the feed stops, and the hardware
 * resets the chip even though esp_restart() can no longer run.
 *
 * Note: rtc_wdt_* (rtc_wdt.h) is NOT this API - it exists only for ESP32 and
 * ESP32-S2. On the C6 the RWDT is reached through hal/wdt_hal.h.
 *
 * On the host build (no ESP-IDF) the guard is a set of inert stubs so the
 * tests link without the WDT HAL.
 */
#ifndef RWDT_GUARD_H
#define RWDT_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool armed;          /* the RWDT is configured and running */
    uint32_t timeout_ms; /* configured backstop timeout */
    uint32_t ticks;      /* configured stage-0 tick count */
    uint32_t feeds;      /* number of accepted feeds since boot */
} rwdt_guard_status_t;

#ifdef ESP_PLATFORM

/* Initialise and enable the RWDT backstop with stage 0 = system reset.
 * Idempotent; call once early in app_main, before the workers start. Returns
 * the ESP-IDF error (ESP_OK on success). A failure is non-fatal: the other
 * watchdogs still cover a software stall. */
esp_err_t rwdt_guard_init(void);

/* Feed the RWDT (reset its counter). Call only when real forward progress was
 * observed; a missing feed is what lets the hardware reset fire. Safe to call
 * before init (no-op). */
void rwdt_guard_feed(void);

/* Snapshot for the console/diagnostics. */
void rwdt_guard_get_status(rwdt_guard_status_t *out);

#else /* host build: inert stubs so the store/tests link without the WDT HAL */

#include <string.h>

static inline esp_err_t rwdt_guard_init(void) { return ESP_OK; }
static inline void rwdt_guard_feed(void) {}
static inline void rwdt_guard_get_status(rwdt_guard_status_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

#endif /* ESP_PLATFORM */

#endif /* RWDT_GUARD_H */
