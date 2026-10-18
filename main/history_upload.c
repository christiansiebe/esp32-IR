#include "history_upload.h"

#include <string.h>

#include "history_csv_import.h"

void history_upload_init(history_upload_t *up, history_upload_flush_fn flush,
                         void *ctx)
{
    if (up == NULL) {
        return;
    }
    memset(up, 0, sizeof(*up));
    up->flush = flush;
    up->ctx = ctx;
    up->first_line = true;
}

static void history_upload_flush_batch(history_upload_t *up)
{
    if (up->batch_count == 0 || up->aborted) {
        up->batch_count = 0;
        return;
    }
    /* The store inserts each record at its time-ordered position, so the batch
     * only needs to be sorted within itself (matches the previous inline
     * handler, and keeps a not-chronological file importable). */
    history_csv_sort(up->batch, up->batch_count);
    size_t imported = 0;
    size_t skipped = 0;
    bool ok = true;
    if (up->flush != NULL) {
        ok = up->flush(up->ctx, up->batch, up->batch_count, &imported, &skipped);
    }
    up->imported += imported;
    up->duplicates += skipped;
    up->batch_count = 0;
    if (!ok) {
        up->aborted = true;
    }
}

static void history_upload_line(history_upload_t *up, const char *line, size_t len)
{
    bool blank = true;
    for (size_t k = 0; k < len; ++k) {
        if (line[k] != ' ' && line[k] != '\t' && line[k] != '\r') {
            blank = false;
            break;
        }
    }
    if (blank) {
        return;
    }
    power_history_record_t record;
    if (!history_csv_parse_line(line, len, &record)) {
        /* The first non-blank line may be the column header; only later bad
         * rows are counted as malformed. */
        if (!up->first_line) {
            ++up->malformed;
        }
        up->first_line = false;
        return;
    }
    up->first_line = false;
    ++up->parsed;
    if (up->have_last && record.interval_start_utc < up->last_utc) {
        up->unsorted = true;
    }
    up->last_utc = record.interval_start_utc;
    up->have_last = true;
    up->batch[up->batch_count++] = record;
    if (up->batch_count == HISTORY_UPLOAD_BATCH) {
        history_upload_flush_batch(up);
    }
}

void history_upload_feed(history_upload_t *up, const char *data, size_t len)
{
    if (up == NULL || data == NULL || up->aborted) {
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == '\n') {
            if (up->line_overflow) {
                ++up->malformed;
            } else {
                history_upload_line(up, up->line, up->line_len);
            }
            up->line_len = 0;
            up->line_overflow = false;
            if (up->aborted) {
                return;
            }
        } else if (up->line_len < sizeof(up->line) - 1) {
            up->line[up->line_len++] = c;
        } else {
            up->line_overflow = true;
        }
    }
}

void history_upload_finish(history_upload_t *up)
{
    if (up == NULL || up->aborted) {
        return;
    }
    if (up->line_overflow) {
        ++up->malformed;
    } else if (up->line_len != 0) {
        history_upload_line(up, up->line, up->line_len);
    }
    up->line_len = 0;
    up->line_overflow = false;
    history_upload_flush_batch(up);
}
