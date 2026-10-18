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
        /* A transient ESP-NOW miss (peer not ready, one lost frame) must not end
         * the session: retry the same cursor a few times before giving up. The
         * cursor is unchanged across the retries, so a page is never skipped. */
        bool fetched = false;
        for (uint32_t attempt = 0; attempt < HISTORY_SYNC_FETCH_ATTEMPTS; ++attempt) {
            memset(&reply, 0, sizeof(reply));
            if (fetch(ctx, resume.after_utc, to_utc, &reply)) {
                fetched = true;
                break;
            }
        }
        if (!fetched) {
            /* Every retry missed: a real page loss that ends the session. Count
             * it once here (not per attempt) so the diagnostic is meaningful. */
            result->fetch_failures++;
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
        int64_t page_last_minute = resume.after_utc;
        for (uint8_t i = 0; i < reply.record_count; ++i) {
            const power_history_record_t *record = &reply.records[i];
            if (record->interval_start_utc > page_last_minute) {
                page_last_minute = record->interval_start_utc;
            }
            if (has(ctx, record->interval_start_utc)) {
                continue; /* receiver already holds this minute */
            }
            /* Dedupe within the batch too: a retried/overlapping page can carry
             * the same minute twice, and has() only knows the store, not the
             * records this batch has not merged yet. */
            bool dup_in_batch = false;
            for (size_t j = 0; j < merge_count; ++j) {
                if (merge_buf[j].interval_start_utc == record->interval_start_utc) {
                    dup_in_batch = true;
                    break;
                }
            }
            if (dup_in_batch) {
                continue;
            }
            merge_buf[merge_count++] = *record;
        }
        size_t merged = 0;
        if (merge_count != 0) {
            merged = merge(ctx, merge_buf, merge_count);
            result->merged += (uint32_t)merged;
        }
        /* Advance the resume cursor only over minutes that are provably stored:
         * records the store already held, plus the candidates the merge actually
         * accepted. merge() enqueues from the front and stops at the first
         * rejection, so a short merge means the tail of the page was dropped
         * (queue full). Leaving the cursor before that minute makes the next
         * session re-pull it instead of skipping data. */
        if (merged == merge_count) {
            history_backfill_cursor_advance(&resume, page_last_minute);
        } else {
            int64_t first_drop = merge_buf[merged].interval_start_utc;
            for (uint8_t i = 0; i < reply.record_count; ++i) {
                int64_t minute = reply.records[i].interval_start_utc;
                if (minute >= first_drop) {
                    break;
                }
                history_backfill_cursor_advance(&resume, minute);
            }
            result->pages++;
            if (store != NULL) {
                store(ctx, resume.after_utc);
            }
            /* Store pressure: stop this session and resume from the saved cursor
             * later rather than racing ahead and dropping minutes. */
            saw_more = true;
            break;
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
