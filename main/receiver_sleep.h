#pragma once

/* Receiver idle light sleep.
 *
 * The receiver is mains-powered but still wastes energy staying fully awake
 * around the clock: between the 30-minute history backfills and any dashboard
 * request there is nothing to do. This module parks the CPU in light sleep
 * whenever the receiver has been *idle* for a while, and wakes it the moment
 * real work appears.
 *
 * "Idle" is measured from actual traffic, not from a timer:
 *   - a real (non-loopback) HTTP connection opens  -> active,
 *   - an inbound ESP-NOW frame arrives             -> active.
 * The web-health self-probe runs over loopback and is deliberately NOT counted,
 * or the guard's own 5 s probe would keep the receiver awake forever.
 *
 * Safety: light sleep is opt-in. It only engages when the persisted runtime
 * config has `sleep on` (the compile-time default stays off, so the receiver
 * behaves exactly as before unless the user asks for it). Even when it engages,
 * a missed ESP-NOW broadcast is not data loss: the sender records every telegram
 * in its own minute history and the backfill pulls it back on the next round.
 *
 * The gate is pure C (host-tested in test_receiver_sleep.c); the PM lock and the
 * HTTP/ESP-NOW hooks live behind ESP_PLATFORM.
 */

#include <stdbool.h>
#include <stdint.h>

/* Pure gate: true when the receiver may sleep now. */
static inline bool receiver_sleep_should_sleep(uint64_t now_ms, uint64_t uptime_ms,
                                               uint64_t last_activity_ms,
                                               uint32_t grace_ms, uint32_t idle_ms)
{
    if (uptime_ms < grace_ms) {
        return false; /* boot / early startup is never a sleep candidate */
    }
    if (now_ms < last_activity_ms) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    return (now_ms - last_activity_ms) >= idle_ms;
}

/* Idle time before the receiver parks the CPU. Long enough that an open
 * dashboard tab (which polls a few times a minute) keeps it awake. */
#define RECEIVER_SLEEP_IDLE_MS (60u * 1000u)
/* Grace after boot before sleeping is allowed (Wi-Fi/HTTP come up first). */
#define RECEIVER_SLEEP_GRACE_MS (30u * 1000u)

#ifdef ESP_PLATFORM

#include "esp_http_server.h"

/* Arm the module. `enabled` mirrors the runtime `sleep` knob: when false the
 * module holds the CPU awake permanently (the previous behaviour). Call once,
 * after Wi-Fi is up. */
void receiver_sleep_init(bool enabled);

/* A real HTTP client connected (not the loopback self-probe). */
void receiver_sleep_note_http(void);
/* An inbound ESP-NOW frame arrived. */
void receiver_sleep_note_espnow(void);

/* Periodic tick (1 s): acquire/release the NO_LIGHT_SLEEP lock from the idle
 * state. Safe to call before init. */
void receiver_sleep_tick(void);

bool receiver_sleep_enabled(void);
bool receiver_sleep_is_sleeping(void);
uint32_t receiver_sleep_idle_age_ms(void);

/* httpd open_fn for the receiver's server (see esp_http_server.h): notes a real
 * HTTP connection as activity, ignoring the loopback web-health self-probe.
 * Must return ESP_OK, or httpd closes the socket immediately. */
esp_err_t receiver_sleep_http_open(httpd_handle_t hd, int sockfd);

#endif /* ESP_PLATFORM */
