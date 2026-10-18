#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Last known wall clock.
 *
 * The receiver gets real time from SNTP, the sender over ESP-NOW from the
 * receiver - but only after the network is up. A boot without a fresh time
 * source (no WLAN, receiver down, both devices restarting together) has no
 * wall clock at all, so the minute recorder's `bucket_start` stays 0 and every
 * sample of that boot is silently dropped. Nothing is written until the clock
 * is valid again, which can be minutes or hours.
 *
 * Fix: the newest wall clock the device ever saw is persisted (NVS) and used to
 * seed the clock on the next boot. The seed is only *approximate* (it is at
 * most the persist interval old and keeps counting on the crystal), so it is
 * deliberately not treated as a trusted time source - it just lets the minute
 * recorder start again immediately. The first real sync (SNTP / ESP-NOW)
 * replaces it.
 *
 * The record format and the clock math are pure C so they can be host-tested
 * without NVS/FreeRTOS. The NVS binding lives in wall_clock_store.c.
 */

#define WALL_CLOCK_MAGIC   0x4B434C57u /* "WLCK" */
#define WALL_CLOCK_VERSION 1u

/* A wall clock below this is not a real date (matches the 2024-01-01 bound the
 * history code uses everywhere) and above the upper bound it is nonsense from a
 * corrupt or wildly wrong source. Both are rejected instead of persisted. */
#define WALL_CLOCK_MIN_UTC 1704067200LL        /* 2024-01-01T00:00:00Z */
#define WALL_CLOCK_MAX_UTC 4102444800LL        /* 2100-01-01T00:00:00Z */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int64_t utc_seconds;
    uint32_t checksum;
} wall_clock_record_t;

/* True when `utc_seconds` is a plausible wall clock (see the bounds above). */
bool wall_clock_utc_is_sane(int64_t utc_seconds);

/* Fill a record for the given time (reserved zeroed, checksum computed). */
void wall_clock_make_record(wall_clock_record_t *record, int64_t utc_seconds);

/* magic + version + sane time + checksum. */
bool wall_clock_record_is_valid(const wall_clock_record_t *record);

/* UTC - uptime as a microsecond offset, the representation the sender keeps.
 * Pure arithmetic, so the "the clock keeps advancing after a reboot" property
 * can be asserted on the host. */
int64_t wall_clock_offset_us_from_utc(int64_t utc_seconds, uint64_t uptime_ms);

/* Inverse of the above: the wall clock in ms for a given uptime. */
int64_t wall_clock_utc_ms_from_offset(int64_t offset_us, uint64_t uptime_ms);

/* Floor a wall clock to a fixed interval (the 60 s minute bucket). A guard
 * against interval <= 0 keeps a misconfigured caller from dividing by zero. */
int64_t wall_clock_bucket_start_utc(int64_t utc_seconds, int64_t interval_seconds);

/* True when the persisted clock should be refreshed: nothing saved yet, or the
 * clock has moved on by at least `interval_seconds`. Also true when the clock
 * went backwards (a fresh sync corrected the seed), so the correction is stored
 * instead of being ignored until the next full interval. */
bool wall_clock_persist_due(int64_t last_saved_utc, int64_t now_utc,
                            int64_t interval_seconds);

/* NVS-backed persistence (implemented in wall_clock_store.c). `save` rejects
 * an insane time so a bad source can never poison the store; `load` returns
 * ESP_OK with *found=false when nothing (valid) is stored. */
esp_err_t wall_clock_store_save(int64_t utc_seconds);
esp_err_t wall_clock_store_load(int64_t *utc_seconds, bool *found);
