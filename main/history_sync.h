#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "power_history.h"

/* One full-store sync session, transport- and store-agnostic so it can be
 * driven by the ESP-NOW path in the firmware and by a mock sender on the host.
 *
 * The receiver mirrors the sender's whole minute ring: it pulls time-ordered
 * pages that follow a resume cursor, merges only the minutes it still misses,
 * and persists the cursor after every page. A radio drop ends the session with
 * the cursor saved, so the next session continues exactly where it stopped.
 */

#define HISTORY_SYNC_PAGE_RECORDS 7u

/* Fetch attempts per page before a session gives up on the link. A single lost
 * ESP-NOW frame (or a peer that is briefly not ready) must not abort the sync. */
#define HISTORY_SYNC_FETCH_ATTEMPTS 3u

typedef struct {
    int64_t oldest_utc;   /* sender's oldest minute (0 when empty) */
    int64_t latest_utc;   /* sender's newest minute (0 when empty) */
    uint8_t record_count;
    bool more;            /* further pages follow */
    power_history_record_t records[HISTORY_SYNC_PAGE_RECORDS];
} history_sync_page_t;

/* Fetch one time-ordered page with interval_start_utc > after_utc. Returns false
 * on a transport failure (the session then stops and keeps the cursor). */
typedef bool (*history_sync_fetch_fn)(void *ctx, int64_t after_utc, int64_t to_utc,
                                      history_sync_page_t *page);

/* True when the receiver already holds that minute (dedupe). */
typedef bool (*history_sync_has_fn)(void *ctx, int64_t minute_utc);

/* Merge the minutes the receiver still misses; returns how many were taken. */
typedef size_t (*history_sync_merge_fn)(void *ctx,
                                        const power_history_record_t *records,
                                        size_t count);

/* Persist the resume cursor after a page (NVS in the firmware). */
typedef void (*history_sync_store_fn)(void *ctx, int64_t cursor);

typedef struct {
    int64_t cursor;            /* resume point after the session */
    uint32_t pages;            /* pages fetched */
    uint32_t merged;           /* minutes merged into the receiver store */
    /* Pages whose fetch exhausted every retry (a real link failure that ended
     * the session). A transient miss that a retry recovered is NOT counted, so
     * the backfill `errors` diagnostic reflects lost pages, not each attempt. */
    uint32_t fetch_failures;
    bool any_round;            /* at least one page round-trip completed */
    bool caught_up;            /* cursor reached the sender's newest minute */
    int64_t sender_oldest_utc;
    int64_t sender_latest_utc;
} history_sync_result_t;

void history_sync_run(history_sync_result_t *result, int64_t start_cursor,
                      int64_t to_utc, uint32_t max_pages,
                      history_sync_fetch_fn fetch, history_sync_has_fn has,
                      history_sync_merge_fn merge, history_sync_store_fn store,
                      void *ctx);
