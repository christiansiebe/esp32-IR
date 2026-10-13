/* End-to-end test of a full-store sync session with a mock sender: the session
 * must mirror the sender's whole minute ring, resume after a simulated link
 * drop, and never duplicate a minute the receiver already holds. Drives the real
 * history_sync_run() with injected transport/store callbacks (no radio, no
 * SPIFFS), so the paging/dedupe/resume logic is exercised as one flow. */

#include <stdio.h>
#include <string.h>

#include "history_sync.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("  FAIL: %s\n", (msg));                                     \
        }                                                                      \
    } while (0)

#define MAX_MINUTES 200

typedef struct {
    int64_t minutes[MAX_MINUTES];
    int count;
    int pages_served;
    int fail_after_pages; /* -1 = never fail */
} mock_sender_t;

typedef struct {
    int64_t minutes[MAX_MINUTES];
    int count;
} mock_store_t;

typedef struct {
    mock_sender_t *sender;
    mock_store_t *store;
    int64_t cursor_stored;
    int store_calls;
} mock_ctx_t;

static bool store_has(const mock_store_t *store, int64_t minute)
{
    for (int i = 0; i < store->count; ++i) {
        if (store->minutes[i] == minute) {
            return true;
        }
    }
    return false;
}

static bool mock_fetch(void *ctx, int64_t after_utc, int64_t to_utc,
                       history_sync_page_t *page)
{
    (void)to_utc;
    mock_ctx_t *c = ctx;
    mock_sender_t *s = c->sender;
    if (s->fail_after_pages >= 0 && s->pages_served >= s->fail_after_pages) {
        return false; /* simulate a radio drop */
    }
    s->pages_served++;

    page->oldest_utc = s->count ? s->minutes[0] : 0;
    page->latest_utc = s->count ? s->minutes[s->count - 1] : 0;
    page->record_count = 0;
    page->more = false;
    for (int i = 0; i < s->count; ++i) {
        if (s->minutes[i] <= after_utc) {
            continue;
        }
        if (page->record_count == HISTORY_SYNC_PAGE_RECORDS) {
            page->more = true;
            break;
        }
        power_history_record_t *r = &page->records[page->record_count++];
        memset(r, 0, sizeof(*r));
        r->interval_start_utc = s->minutes[i];
        r->sequence = (uint32_t)(i + 1);
    }
    return true;
}

static bool mock_has(void *ctx, int64_t minute_utc)
{
    mock_ctx_t *c = ctx;
    return store_has(c->store, minute_utc);
}

static size_t mock_merge(void *ctx, const power_history_record_t *records, size_t count)
{
    mock_ctx_t *c = ctx;
    size_t added = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t minute = records[i].interval_start_utc;
        if (store_has(c->store, minute)) {
            continue;
        }
        int pos = c->store->count;
        for (int j = 0; j < c->store->count; ++j) {
            if (c->store->minutes[j] > minute) {
                pos = j;
                break;
            }
        }
        for (int j = c->store->count; j > pos; --j) {
            c->store->minutes[j] = c->store->minutes[j - 1];
        }
        c->store->minutes[pos] = minute;
        c->store->count++;
        ++added;
    }
    return added;
}

static void mock_store_cursor(void *ctx, int64_t cursor)
{
    mock_ctx_t *c = ctx;
    c->cursor_stored = cursor;
    c->store_calls++;
}

static void fill_sender(mock_sender_t *s, int64_t base, int n)
{
    memset(s, 0, sizeof(*s));
    s->fail_after_pages = -1;
    s->count = n;
    for (int i = 0; i < n; ++i) {
        s->minutes[i] = base + i * 60;
    }
}

static bool store_matches(const mock_store_t *store, const mock_sender_t *sender)
{
    if (store->count != sender->count) {
        return false;
    }
    for (int i = 0; i < store->count; ++i) {
        if (store->minutes[i] != sender->minutes[i]) {
            return false;
        }
    }
    return true;
}

static void test_full_mirror_from_empty(void)
{
    printf("test_full_mirror_from_empty\n");
    const int64_t base = 7000000000LL;
    mock_sender_t sender;
    fill_sender(&sender, base, 30);
    mock_store_t store = {0};
    mock_ctx_t ctx = {.sender = &sender, .store = &store};

    history_sync_result_t result;
    history_sync_run(&result, 0, base + 30 * 60, 64, mock_fetch, mock_has, mock_merge,
                     mock_store_cursor, &ctx);

    CHECK(result.merged == 30, "all 30 minutes merged");
    CHECK(result.pages == 5, "30 records over 5 pages of 7");
    CHECK(result.caught_up, "session reports caught up");
    CHECK(result.cursor == base + 29 * 60, "cursor at the sender's newest minute");
    CHECK(store_matches(&store, &sender), "receiver store mirrors the sender");
    CHECK(ctx.cursor_stored == result.cursor, "cursor persisted after the last page");
}

static void test_resume_after_link_drop(void)
{
    printf("test_resume_after_link_drop\n");
    const int64_t base = 7100000000LL;
    mock_sender_t sender;
    fill_sender(&sender, base, 30);
    mock_store_t store = {0};
    mock_ctx_t ctx = {.sender = &sender, .store = &store};

    /* Drop the link after two pages. */
    sender.fail_after_pages = 2;
    history_sync_result_t first;
    history_sync_run(&first, 0, base + 30 * 60, 64, mock_fetch, mock_has, mock_merge,
                     mock_store_cursor, &ctx);
    CHECK(first.merged == 14, "only the two served pages merged");
    CHECK(!first.caught_up, "not caught up after the drop");
    CHECK(first.cursor == base + 13 * 60, "cursor stops at the last served minute");
    CHECK(store.count == 14, "store holds the 14 served minutes");

    /* Link is back: resume from the persisted cursor and finish. */
    sender.fail_after_pages = -1;
    history_sync_result_t second;
    history_sync_run(&second, first.cursor, base + 30 * 60, 64, mock_fetch, mock_has,
                     mock_merge, mock_store_cursor, &ctx);
    CHECK(second.caught_up, "resumed session catches up");
    CHECK(store_matches(&store, &sender), "resume completes the mirror with no holes");
    CHECK(store.count == 30, "no duplicates after resume");
}

static void test_dedupe_against_local_data(void)
{
    printf("test_dedupe_against_local_data\n");
    const int64_t base = 7200000000LL;
    mock_sender_t sender;
    fill_sender(&sender, base, 30);
    mock_store_t store = {0};
    mock_ctx_t ctx = {.sender = &sender, .store = &store};

    /* The receiver already recorded the first 10 minutes live. */
    for (int i = 0; i < 10; ++i) {
        store.minutes[store.count++] = base + i * 60;
    }
    history_sync_result_t result;
    history_sync_run(&result, 0, base + 30 * 60, 64, mock_fetch, mock_has, mock_merge,
                     mock_store_cursor, &ctx);
    CHECK(result.merged == 20, "only the 20 missing minutes merged");
    CHECK(store_matches(&store, &sender), "store still mirrors the sender");
    CHECK(store.count == 30, "local minutes were not duplicated");
}

static void test_already_caught_up(void)
{
    printf("test_already_caught_up\n");
    const int64_t base = 7300000000LL;
    mock_sender_t sender;
    fill_sender(&sender, base, 10);
    mock_store_t store = {0};
    mock_ctx_t ctx = {.sender = &sender, .store = &store};
    for (int i = 0; i < 10; ++i) {
        store.minutes[store.count++] = base + i * 60;
    }
    int pages_before = sender.pages_served;
    history_sync_result_t result;
    history_sync_run(&result, base + 9 * 60, base + 10 * 60, 64, mock_fetch, mock_has,
                     mock_merge, mock_store_cursor, &ctx);
    CHECK(result.merged == 0, "nothing merged when already up to date");
    CHECK(result.caught_up, "already up to date reports caught up");
    CHECK(sender.pages_served == pages_before + 1, "one empty page ends the session");
}

static void test_drop_before_first_page(void)
{
    printf("test_drop_before_first_page\n");
    const int64_t base = 7400000000LL;
    mock_sender_t sender;
    fill_sender(&sender, base, 10);
    sender.fail_after_pages = 0;
    mock_store_t store = {0};
    mock_ctx_t ctx = {.sender = &sender, .store = &store};
    history_sync_result_t result;
    history_sync_run(&result, 0, base + 10 * 60, 64, mock_fetch, mock_has, mock_merge,
                     mock_store_cursor, &ctx);
    CHECK(!result.any_round, "no round completed");
    CHECK(result.cursor == 0, "cursor unchanged on a first-page drop");
    CHECK(store.count == 0, "nothing merged");
}

int main(void)
{
    test_full_mirror_from_empty();
    test_resume_after_link_drop();
    test_dedupe_against_local_data();
    test_already_caught_up();
    test_drop_before_first_page();
    printf("test_history_sync: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
