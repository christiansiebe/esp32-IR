#pragma once

/*
 * Day / month energy totals derived from the stored minute records.
 *
 * The minute ring only holds ~20 days (POWER_HISTORY_CAPACITY). Long-term
 * graphs (per day, per month) would otherwise have to re-scan the whole ring
 * every time, and anything older than the ring is gone. This module keeps a
 * compact running total per UTC day and per UTC month, so the dashboard can show
 * "today" / "this month" and a longer history cheaply.
 *
 * Each minute record carries an average power; one record is exactly one minute,
 * so the exact energy is average_milliwatts * 1 min. Totals are accumulated in
 * milliwatt-minutes (int64, no floating point drift) and converted to Wh only
 * when rendered.
 *
 * Only *measured* minutes are counted: a boot marker (reason != NONE, or a
 * record with no samples) is not a measurement and contributes nothing.
 *
 * The accumulator is pure C (no ESP-IDF), so the day/month split, the rollover,
 * the exclusion of boot markers and the on-disk round-trip are host-tested
 * (test/test_energy_totals.c). The NVS binding lives in energy_totals_store.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "power_history.h"

/* How many UTC days / months the running totals cover. The day ring is a little
 * larger than the minute ring (~20 days) so a full store never evicts a day it
 * still holds minutes for; the month ring covers a year and a half. Both keep
 * the blob tiny (a few hundred bytes) so it fits one NVS value. */
#define ENERGY_TOTALS_DAY_SLOTS   40u
#define ENERGY_TOTALS_MONTH_SLOTS 18u

#define ENERGY_TOTALS_MAGIC   0x4D555345u /* "ESUM" */
#define ENERGY_TOTALS_VERSION 1u

typedef struct __attribute__((packed)) {
    int64_t day_utc;      /* UTC midnight of the day (0 = unused slot) */
    int64_t mw_minutes;   /* sum of average_milliwatts over the day's minutes */
    uint32_t minute_count;
    uint32_t reserved;
} energy_day_slot_t;

typedef struct __attribute__((packed)) {
    int32_t month_index;  /* year * 12 + (month - 1); negative/0 = unused slot */
    int32_t reserved;
    int64_t mw_minutes;
    uint32_t minute_count;
    uint32_t reserved2;
} energy_month_slot_t;

typedef struct {
    int64_t folded_through_utc; /* newest minute folded in; 0 = nothing yet */
    energy_day_slot_t days[ENERGY_TOTALS_DAY_SLOTS];
    energy_month_slot_t months[ENERGY_TOTALS_MONTH_SLOTS];
} energy_totals_t;

/* On-disk blob (NVS value): a versioned, CRC32-protected image of the totals. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int64_t folded_through_utc;
    energy_day_slot_t days[ENERGY_TOTALS_DAY_SLOTS];
    energy_month_slot_t months[ENERGY_TOTALS_MONTH_SLOTS];
    uint32_t checksum;
} energy_totals_blob_t;

/* UTC midnight of the day containing `utc_seconds`. Pure, floors to the day. */
int64_t energy_totals_day_start_utc(int64_t utc_seconds);

/* Calendar-month index (year * 12 + month-1) for `utc_seconds`. Pure, so the
 * rollover at a year boundary is host-testable without a clock. */
int32_t energy_totals_month_index(int64_t utc_seconds);

/* Clear every slot (folded_through_utc = 0). */
void energy_totals_reset(energy_totals_t *totals);

/* Fold one stored minute into the day/month totals. Returns true when the
 * record counted (a measured minute); false for a boot marker, an invalid
 * minute or a NULL argument. The newest counted minute advances
 * folded_through_utc so a later "fold the ring from here" pass does not double
 * count. */
bool energy_totals_add(energy_totals_t *totals, const power_history_record_t *record);

/* Total energy in Wh for the UTC day containing `utc_seconds` (0 if absent). */
double energy_totals_day_wh(const energy_totals_t *totals, int64_t utc_seconds);

/* Total energy in Wh for the calendar month containing `utc_seconds`. */
double energy_totals_month_wh(const energy_totals_t *totals, int64_t utc_seconds);

/* Fill *blob from *totals (reserved zeroed, checksum computed). */
void energy_totals_make_blob(const energy_totals_t *totals, energy_totals_blob_t *blob);

/* magic + version + checksum. A foreign/corrupt blob is rejected so a bad NVS
 * value can never poison the running totals. */
bool energy_totals_blob_is_valid(const energy_totals_blob_t *blob);

/* Load *totals from a validated blob. Returns false (totals left cleared) for a
 * NULL or invalid blob. */
bool energy_totals_from_blob(const energy_totals_blob_t *blob, energy_totals_t *totals);

/* NVS-backed persistence (implemented in energy_totals_store.c). `save` writes
 * the blob; `load` returns ESP_OK with *found=false when nothing (valid) is
 * stored, so a corrupt value never poisons the running totals. */
esp_err_t energy_totals_store_save(const energy_totals_t *totals);
esp_err_t energy_totals_store_load(energy_totals_t *totals, bool *found);
