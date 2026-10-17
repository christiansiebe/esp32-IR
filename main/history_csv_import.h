#pragma once

#include <stddef.h>

#include "power_history.h"

/* Pure CSV import for the receiver's "restore from backup" upload.
 *
 * The accepted format is exactly what /api/power-history.csv exports:
 *
 *   sequence,interval_start_ms,average_w,sample_count,reason
 *
 * (CRLF or LF, an optional header line, ISO-8601 UTC timestamp such as
 * 2026-10-13T09:00:00Z). `sequence` is ignored — the store renumbers every
 * record, so a backup from any device restores cleanly. `reason` is optional and
 * defaults to "none"; the machine tokens match power_history_reason_token().
 *
 * Rows that do not parse are dropped and counted in *skipped_out. No ESP-IDF
 * dependency, so the parser is host-tested (test/test_history_csv_import.c).
 *
 * Returns the number of records written to `out` (never more than `capacity`);
 * *skipped_out gets the number of malformed rows plus any row beyond capacity. */
size_t history_csv_parse(const char *text, size_t len,
                         power_history_record_t *out, size_t capacity,
                         size_t *skipped_out);

/* Sort records ascending by interval_start_utc, the order
 * power_history_import_sorted() requires. */
void history_csv_sort(power_history_record_t *records, size_t count);

/* Parse a single CSV line (a trailing newline is optional). Returns true and
 * fills *record when the row is well-formed. Exposed so the upload handler can
 * stream a large backup line by line instead of buffering it all — a full store
 * is ~29000 rows, which does not fit in the ESP32 heap. */
bool history_csv_parse_line(const char *line, size_t len,
                            power_history_record_t *record);
