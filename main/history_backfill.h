#pragma once

/* Receiver-side merge bookkeeping for the ESP-NOW history backfill.
 *
 * The sender keeps its own persistent minute history (power_history runs on
 * both roles), so after a radio gap or a restart the receiver can pull the
 * minutes it missed and merge them into its own store. This module owns the
 * "which minutes do I still miss" decision and is deliberately free of any
 * ESP-IDF dependency, so it is host-tested (test/test_history_backfill.c).
 *
 * The window is divided into fixed 60 s buckets (one per minute record). A
 * bucket is "present" once the receiver either recorded that minute live or
 * merged a record from the sender; a sender record is merged only when its
 * minute is inside the window and still absent. That is the dedupe rule: the
 * receiver's live data is never overwritten, the sender only fills the holes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HISTORY_BACKFILL_BUCKET_SECONDS 60
/* How far back a backfill session looks. A full store is ~20 days / 29000
 * records; pulling all of it over ESP-NOW would take hours, so the automatic
 * session only repairs the recent past and leaves a deep gap to a manual pull. */
#define HISTORY_BACKFILL_WINDOW_SECONDS (3 * 24 * 60 * 60)

typedef struct {
    int64_t from_utc;      /* inclusive */
    int64_t to_utc;        /* exclusive */
    size_t bucket_count;   /* (to_utc - from_utc) / BUCKET_SECONDS */
    uint8_t *present;      /* caller storage, bucket_count bytes */
} history_backfill_t;

/* Set the window and clear the presence map. Returns false on a bad window or
 * when `storage` cannot hold bucket_count bytes. */
bool history_backfill_reset(history_backfill_t *bf, int64_t from_utc, int64_t to_utc,
                            uint8_t *storage, size_t storage_len);

/* True and *out_index set when interval_start_utc falls inside the window. */
bool history_backfill_index(const history_backfill_t *bf, int64_t interval_start_utc,
                            size_t *out_index);

/* Record that the receiver already holds that minute (its own live data). */
void history_backfill_mark_present(history_backfill_t *bf, int64_t interval_start_utc);

bool history_backfill_is_present(const history_backfill_t *bf, int64_t interval_start_utc);

/* The merge decision for one sender record: inside the window and not held. */
bool history_backfill_should_merge(const history_backfill_t *bf,
                                   int64_t interval_start_utc);

/* Minutes in the window the receiver still misses (shrinks as records merge). */
uint32_t history_backfill_missing_count(const history_backfill_t *bf);
