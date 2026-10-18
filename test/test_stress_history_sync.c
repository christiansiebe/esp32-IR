/* Stress simulation of the receiver's history backfill.
 *
 * Drives the real history_sync_run() against a large mock sender ring under
 * adverse conditions: random ESP-NOW link drops (a session ends early), random
 * store pressure (the merge queue drops the tail of a page), and a receiver
 * that starts with a partially filled ring. Repeated sessions must converge to
 * an exact mirror of the sender: every sender minute present exactly once, no
 * minute lost, no duplicate, and the persisted cursor only ever points at a
 * minute that is provably stored. This is the "no data lost" guarantee under
 * stress, without a radio or SPIFFS. */

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

#define MAX_MINUTES 4096
#define RING_MINUTES 2000

/* Deterministic PRNG so a failure is reproducible. */
static unsigned g_seed;
static unsigned next_rand(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0x7FFFFFFFu;
}

typedef struct {
    int64_t minutes[MAX_MINUTES];
    int count;
    int pages_served;
    int drop_after_pages;   /* -1 = never; else drop the link after N pages */
    int transient_misses;   /* consecutive fetch misses before a page lands */
} mock_sender_t;

typedef struct {
    int64_t minutes[MAX_MINUTES];
    int count;
} mock_store_t;

typedef struct {
    mock_sender_t *sender;
    mock_store_t *store;
    int64_t cursor_stored;
    int merge_accept_limit; /* >0: accept only this many per merge call */
    int fetch_calls;
    int merge_calls;
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
    ++c->fetch_calls;
    if (s->transient_misses > 0) {
        s->transient_misses--;
        return false;
    }
    if (s->drop_after_pages >= 0 && s->pages_served >= s->drop_after_pages) {
        return false;
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
    ++c->merge_calls;
    if (c->merge_accept_limit > 0 && (size_t)c->merge_accept_limit < count) {
        count = (size_t)c->merge_accept_limit; /* full store queue: tail dropped */
    }
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
}

/* Minutes the store must contain: every sender minute not already held when the
 * run began (the store is seeded with a prefix, the rest must be pulled). */
static int count_missing(const mock_store_t *store, const mock_sender_t *sender)
{
    int missing = 0;
    for (int i = 0; i < sender->count; ++i) {
        if (!store_has(store, sender->minutes[i])) {
            ++missing;
        }
    }
    return missing;
}

static bool store_has_duplicates(const mock_store_t *store)
{
    for (int i = 0; i < store->count; ++i) {
        for (int j = i + 1; j < store->count; ++j) {
            if (store->minutes[i] == store->minutes[j]) {
                return true;
            }
        }
    }
    return false;
}

/* One adverse scenario: a big ring, a seeded partial store, and a chain of
 * sessions where each may drop early and may face store pressure. */
static void run_scenario(unsigned seed, int seeded_prefix)
{
    g_seed = seed;
    const int64_t base = 8000000000LL;
    static mock_sender_t sender;
    static mock_store_t store;
    static mock_ctx_t ctx;

    memset(&sender, 0, sizeof(sender));
    memset(&store, 0, sizeof(store));
    memset(&ctx, 0, sizeof(ctx));
    sender.drop_after_pages = -1;
    sender.count = RING_MINUTES;
    for (int i = 0; i < RING_MINUTES; ++i) {
        sender.minutes[i] = base + (int64_t)i * 60;
    }
    for (int i = 0; i < seeded_prefix && i < RING_MINUTES; ++i) {
        store.minutes[store.count++] = sender.minutes[i];
    }
    ctx.sender = &sender;
    ctx.store = &store;

    int sessions = 0;
    uint32_t total_fetch_failures = 0;
    int64_t cursor = seeded_prefix > 0 ? sender.minutes[seeded_prefix - 1] : 0;
    int64_t prev_cursor = -1;
    int guard = 0;
    while (count_missing(&store, &sender) > 0 && guard++ < 200) {
        /* Random adversity for this session. */
        int drop = (int)(next_rand() % 6);          /* drop early 0..5 pages, or never */
        sender.drop_after_pages = (drop == 0) ? -1 : drop;
        sender.transient_misses = (int)(next_rand() % 3);
        ctx.merge_accept_limit = (next_rand() % 5 == 0) ? (int)(next_rand() % 4 + 1) : 0;

        history_sync_result_t result;
        history_sync_run(&result, cursor, base + RING_MINUTES * 60, 64,
                         mock_fetch, mock_has, mock_merge, mock_store_cursor, &ctx);
        ++sessions;
        total_fetch_failures += result.fetch_failures;

        /* The cursor must never rewind and must never exceed the smallest
         * missing minute (otherwise a minute would be skipped). */
        CHECK(result.cursor >= prev_cursor, "cursor never rewinds");
        int64_t smallest_missing = 0;
        for (int i = 0; i < sender.count; ++i) {
            if (!store_has(&store, sender.minutes[i])) {
                smallest_missing = sender.minutes[i];
                break;
            }
        }
        if (smallest_missing != 0) {
            CHECK(result.cursor < smallest_missing,
                  "cursor stays before the first missing minute (no skip)");
        }
        prev_cursor = result.cursor;
        cursor = result.cursor;
        CHECK(cursor == ctx.cursor_stored, "persisted cursor matches the session cursor");
    }

    CHECK(count_missing(&store, &sender) == 0, "no minute lost after the stress run");
    CHECK(!store_has_duplicates(&store), "no duplicate minute after the stress run");
    CHECK(store.count == RING_MINUTES, "store mirrors the sender exactly");
    CHECK(sessions < 200, "sessions converge (no livelock)");
    /* Each fetch failure ends its session, so there can never be more failures
     * than fetch calls (a retry that recovered is not counted). */
    CHECK(total_fetch_failures <= (uint32_t)ctx.fetch_calls,
          "fetch failures never exceed the fetch attempts made");
    printf("  seed %u: %d minutes, prefix %d, %d sessions, %d fetch, %d merge -> OK\n",
           seed, RING_MINUTES, seeded_prefix, sessions, ctx.fetch_calls, ctx.merge_calls);
}

int main(void)
{
    printf("stress history-sync: adverse link + store pressure\n");
    run_scenario(1u, 0);
    run_scenario(2u, 500);
    run_scenario(3u, 1999);
    run_scenario(4u, 1000);
    run_scenario(5u, 137);
    run_scenario(12345u, 777);
    printf("stress history-sync: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
