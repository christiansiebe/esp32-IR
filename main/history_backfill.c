#include "history_backfill.h"

#include <string.h>

bool history_backfill_reset(history_backfill_t *bf, int64_t from_utc, int64_t to_utc,
                            uint8_t *storage, size_t storage_len)
{
    if (bf == NULL || storage == NULL || to_utc <= from_utc) {
        return false;
    }
    int64_t span = to_utc - from_utc;
    /* Align the start down to a whole minute so bucket indices are stable even
     * when the caller passes a not-yet-rounded "now". */
    from_utc -= from_utc % HISTORY_BACKFILL_BUCKET_SECONDS;
    size_t bucket_count = (size_t)((span + HISTORY_BACKFILL_BUCKET_SECONDS - 1) /
                                   HISTORY_BACKFILL_BUCKET_SECONDS);
    if (bucket_count == 0 || bucket_count > storage_len) {
        return false;
    }
    bf->from_utc = from_utc;
    bf->to_utc = to_utc;
    bf->bucket_count = bucket_count;
    bf->present = storage;
    memset(storage, 0, bucket_count);
    return true;
}

bool history_backfill_index(const history_backfill_t *bf, int64_t interval_start_utc,
                            size_t *out_index)
{
    if (bf == NULL || bf->present == NULL || interval_start_utc < bf->from_utc ||
        interval_start_utc >= bf->to_utc) {
        return false;
    }
    size_t index = (size_t)((interval_start_utc - bf->from_utc) /
                            HISTORY_BACKFILL_BUCKET_SECONDS);
    if (index >= bf->bucket_count) {
        return false;
    }
    if (out_index != NULL) {
        *out_index = index;
    }
    return true;
}

void history_backfill_mark_present(history_backfill_t *bf, int64_t interval_start_utc)
{
    size_t index;
    if (history_backfill_index(bf, interval_start_utc, &index)) {
        bf->present[index] = 1;
    }
}

bool history_backfill_is_present(const history_backfill_t *bf, int64_t interval_start_utc)
{
    size_t index;
    return history_backfill_index(bf, interval_start_utc, &index) &&
           bf->present[index] != 0;
}

bool history_backfill_should_merge(const history_backfill_t *bf,
                                   int64_t interval_start_utc)
{
    size_t index;
    if (!history_backfill_index(bf, interval_start_utc, &index)) {
        return false;
    }
    return bf->present[index] == 0;
}

uint32_t history_backfill_missing_count(const history_backfill_t *bf)
{
    if (bf == NULL || bf->present == NULL) {
        return 0;
    }
    uint32_t missing = 0;
    for (size_t i = 0; i < bf->bucket_count; ++i) {
        if (bf->present[i] == 0) {
            ++missing;
        }
    }
    return missing;
}
