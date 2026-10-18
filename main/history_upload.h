#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "power_history.h"

/* Streaming CSV-restore accumulator (pure C, host-tested).
 *
 * The receiver's "restore from backup" upload can be ~29000 rows, far more than
 * fits in the ESP32 heap, so the body is streamed and parsed line by line. This
 * module owns the byte->line->sorted-batch machinery and the per-upload
 * statistics; it has no ESP-IDF dependency so the batching (partial lines across
 * reads, the optional header, the sorted 64-row flush) is exercised on the host.
 *
 * Every time a full batch is ready (or the input ends) the module calls the
 * caller's `flush` callback, which hands the sorted records to the store. That
 * keeps the actual SPIFFS write out of this module, so the firmware can route it
 * through the single history writer (the backfill worker) instead of the HTTP
 * task. The callback returns whether the batch was accepted; returning false
 * aborts the upload (the caller decides why, e.g. a store error).
 *
 * The accepted format is exactly what /api/power-history.csv exports; see
 * history_csv_import.h. `sequence` is ignored (the store renumbers).
 */

/* Rows per flush. Small enough that one store lock hold stays short (the writer
 * yields between batches), large enough to amortise the per-batch overhead. */
#define HISTORY_UPLOAD_BATCH 64u

/* Longest CSV line kept; a longer line is counted malformed and dropped. */
#define HISTORY_UPLOAD_LINE_MAX 192u

/* Called with a full, time-sorted batch. Fill *imported_out / *skipped_out with
 * the store's answer. Return false to abort the upload. */
typedef bool (*history_upload_flush_fn)(void *ctx, power_history_record_t *records,
                                        size_t count, size_t *imported_out,
                                        size_t *skipped_out);

typedef struct {
    history_upload_flush_fn flush;
    void *ctx;

    power_history_record_t batch[HISTORY_UPLOAD_BATCH];
    size_t batch_count;

    char line[HISTORY_UPLOAD_LINE_MAX];
    size_t line_len;
    bool line_overflow;

    size_t parsed;      /* well-formed data rows seen */
    size_t imported;    /* rows the store took */
    size_t duplicates;  /* rows the store skipped as already present */
    size_t malformed;   /* unparsable rows (after the optional header) */

    int64_t last_utc;
    bool have_last;
    bool unsorted;      /* input rows were not in ascending minute order */
    bool first_line;    /* the first non-blank line may be the column header */
    bool aborted;       /* a flush was rejected; stop accepting input */
} history_upload_t;

void history_upload_init(history_upload_t *up, history_upload_flush_fn flush,
                         void *ctx);

/* Feed a chunk of body bytes. Complete lines are consumed; a trailing partial
 * line is buffered until the next feed (or history_upload_finish). */
void history_upload_feed(history_upload_t *up, const char *data, size_t len);

/* Flush the trailing partial line and any pending batch. Call once after the
 * last history_upload_feed(). */
void history_upload_finish(history_upload_t *up);
