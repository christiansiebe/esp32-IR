#include "history_sync.h"

#include <string.h>

#include "history_backfill.h"

void history_sync_run(history_sync_result_t *result, int64_t start_cursor,
                      int64_t to_utc, uint32_t max_pages,
                      history_sync_fetch_fn fetch, history_sync_has_fn has,
                      history_sync_merge_fn merge, history_sync_store_fn store,
                      void *ctx)
{
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    if (fetch == NULL || has == NULL || merge == NULL || to_utc <= 0) {
        return;
    }

    history_backfill_cursor_t resume;
    resume.after_utc = start_cursor < 0 ? 0 : start_cursor;

    bool saw_more = false;
    for (uint32_t page = 0; page < max_pages; ++page) {
        history_sync_page_t reply;
        memset(&reply, 0, sizeof(reply));
        if (!fetch(ctx, resume.after_utc, to_utc, &reply)) {
            break; /* link drop: cursor already saved up to here */
        }
        result->any_round = true;
        result->sender_oldest_utc = reply.oldest_utc;
        result->sender_latest_utc = reply.latest_utc;

        if (reply.record_count > HISTORY_SYNC_PAGE_RECORDS) {
            break;
        }
        if (reply.record_count == 0) {
            saw_more = false;
            break;
        }

        power_history_record_t merge_buf[HISTORY_SYNC_PAGE_RECORDS];
        size_t merge_count = 0;
        for (uint8_t i = 0; i < reply.record_count; ++i) {
            const power_history_record_t *record = &reply.records[i];
            history_backfill_cursor_advance(&resume, record->interval_start_utc);
            if (has(ctx, record->interval_start_utc)) {
                continue; /* receiver already holds this minute */
            }
            merge_buf[merge_count++] = *record;
        }
        if (merge_count != 0) {
            result->merged += (uint32_t)merge(ctx, merge_buf, merge_count);
        }
        result->pages++;
        if (store != NULL) {
            store(ctx, resume.after_utc);
        }
        saw_more = reply.more;
        if (!saw_more) {
            break;
        }
    }

    result->cursor = resume.after_utc;
    result->caught_up = !saw_more && result->sender_latest_utc != 0 &&
                        history_backfill_caught_up(&resume, result->sender_latest_utc);
}
