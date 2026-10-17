/* Virtual link simulator: drive the REAL history_sync_run() against a scripted
 * sender + a lossy ESP-NOW link, so a new backfill/sleep feature can be checked
 * on the host before it is ever flashed.
 *
 * The scenarios answer the questions that used to need a serial log:
 *   1. A clean link mirrors the sender's whole ring.
 *   2. A sleepy/misbehaving link (dropped fetches) must NOT lose a minute: the
 *      session stops with the cursor saved and the next session resumes.
 *   3. A clock-less sender (its newest minute is far behind "now") must still
 *      report caught_up once the receiver holds everything. This guards the
 *      regression where the sender reported a synthetic latest_utc = now - 120,
 *      which kept caught_up false forever and pinned the receiver to a 10 s poll.
 */

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
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);         \
        }                                                                      \
    } while (0)

#define MAX_MINUTES 400

typedef struct {
    int64_t minutes[MAX_MINUTES];
    int count;
    bool synthetic_latest; /* reproduce the old bug: latest_utc = now - 120 */
    int64_t now_utc;
    int fail_after_pages; /* serve this many pages, then fail (link down) */
    int pages_served;
    int fetch_calls;
} sender_t;

typedef struct {
    int64_t minutes[MAX_MINUTES];
    int count;
} receiver_t;

typedef struct {
    sender_t *sender;
    receiver_t *receiver;
    int64_t cursor;
    int stores;
} ctx_t;

static bool recv_has(const receiver_t *r, int64_t minute)
{
    for (int i = 0; i < r->count; ++i) {
        if (r->minutes[i] == minute) {
            return true;
        }
    }
    return false;
}

static void sender_fill(sender_t *s, int64_t first, int count)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < count; ++i) {
        s->minutes[i] = first + (int64_t)i * 60;
    }
    s->count = count;
    s->now_utc = first + (int64_t)(count + 500) * 60; /* "now" well past the tail */
    s->fail_after_pages = -1; /* link stays up */
}

/* Mirrors the sender's build_diag_history_response(): time-ordered page with
 * interval_start_utc > after_utc, MORE set while further records follow. */
static bool fetch_page(void *ctx, int64_t after_utc, int64_t to_utc,
                       history_sync_page_t *page)
{
    (void)to_utc;
    ctx_t *x = (ctx_t *)ctx;
    sender_t *s = x->sender;
    ++s->fetch_calls;
    if (s->fail_after_pages >= 0 && s->pages_served >= s->fail_after_pages) {
        return false; /* link down: the session must stop, cursor kept */
    }
    memset(page, 0, sizeof(*page));
    page->oldest_utc = s->count != 0 ? s->minutes[0] : 0;
    page->latest_utc = s->synthetic_latest
                           ? s->now_utc - 120
                           : (s->count != 0 ? s->minutes[s->count - 1] : 0);
    int n = 0;
    for (int i = 0; i < s->count; ++i) {
        if (s->minutes[i] <= after_utc) {
            continue;
        }
        if (n == (int)HISTORY_SYNC_PAGE_RECORDS) {
            page->more = true;
            break;
        }
        power_history_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.interval_start_utc = s->minutes[i];
        rec.sequence = (uint32_t)(i + 1);
        rec.format_version = POWER_HISTORY_VERSION;
        rec.reason = POWER_HISTORY_REASON_NONE;
        page->records[n++] = rec;
    }
    page->record_count = (uint8_t)n;
    ++s->pages_served;
    return true;
}

static bool has_minute(void *ctx, int64_t minute)
{
    return recv_has(((ctx_t *)ctx)->receiver, minute);
}

static size_t merge_minutes(void *ctx, const power_history_record_t *records, size_t count)
{
    ctx_t *x = (ctx_t *)ctx;
    size_t merged = 0;
    for (size_t i = 0; i < count; ++i) {
        if (recv_has(x->receiver, records[i].interval_start_utc)) {
            continue;
        }
        x->receiver->minutes[x->receiver->count++] = records[i].interval_start_utc;
        ++merged;
    }
    return merged;
}

static void store_cursor(void *ctx, int64_t cursor)
{
    ctx_t *x = (ctx_t *)ctx;
    x->cursor = cursor;
    ++x->stores;
}

static void run_session(ctx_t *x, int64_t start_cursor, history_sync_result_t *res)
{
    history_sync_run(res, start_cursor, INT64_MAX /* full sync: no upper bound */,
                     HISTORY_SYNC_PAGE_RECORDS * 64, fetch_page, has_minute,
                     merge_minutes, store_cursor, x);
}

static void scenario_clean_mirror(void)
{
    printf("scenario: clean link mirrors the whole ring\n");
    sender_t s;
    sender_fill(&s, 1000000, 50);
    receiver_t r;
    memset(&r, 0, sizeof(r));
    ctx_t x = {.sender = &s, .receiver = &r, .cursor = 0};

    history_sync_result_t res;
    run_session(&x, 0, &res);

    CHECK(res.merged == 50, "all 50 minutes merged");
    CHECK(res.caught_up, "caught_up once the ring is mirrored");
    CHECK(x.cursor == s.minutes[49], "cursor parked at the newest minute");
    CHECK(r.count == 50, "receiver holds every minute");
}

static void scenario_sleepy_link_resumes(void)
{
    printf("scenario: sleepy link drops fetches, no minute is lost\n");
    sender_t s;
    sender_fill(&s, 1000000, 80);
    receiver_t r;
    memset(&r, 0, sizeof(r));
    ctx_t x = {.sender = &s, .receiver = &r, .cursor = 0};

    /* The radio goes quiet after a few pages: enough misses to defeat the
     * per-page retries and end the session with the cursor saved. */
    s.fail_after_pages = 4;
    history_sync_result_t res1;
    run_session(&x, 0, &res1);
    int64_t cursor_after_drop = x.cursor;
    CHECK(!res1.caught_up, "session stopped early while the link was down");
    CHECK(cursor_after_drop > 0, "cursor saved past the pages that got through");
    CHECK(r.count > 0 && r.count < 80, "partial mirror before the drop");

    /* Link recovers: the next session resumes from the saved cursor. */
    s.fail_after_pages = -1;
    history_sync_result_t res2;
    run_session(&x, cursor_after_drop, &res2);

    CHECK(res2.caught_up, "caught up after the link recovered");
    CHECK(r.count == 80, "every sender minute is present (none lost, none duped)");
    CHECK(x.cursor == s.minutes[79], "cursor advanced to the newest minute");
    int dupes = 0;
    for (int i = 0; i < r.count; ++i) {
        for (int j = i + 1; j < r.count; ++j) {
            if (r.minutes[i] == r.minutes[j]) {
                ++dupes;
            }
        }
    }
    CHECK(dupes == 0, "no duplicated minute in the receiver store");
}

static void scenario_clockless_sender_caught_up(void)
{
    printf("scenario: clock-less sender still reports caught_up (real horizon)\n");
    sender_t s;
    sender_fill(&s, 1000000, 30); /* tail is ~500 min behind "now" */
    receiver_t r;
    memset(&r, 0, sizeof(r));
    ctx_t x = {.sender = &s, .receiver = &r, .cursor = 0};

    history_sync_result_t res;
    run_session(&x, 0, &res);
    CHECK(res.caught_up, "real latest_utc lets caught_up become true");
    CHECK(x.cursor == s.minutes[29], "cursor reaches the real tail");

    /* Regression guard: the old synthetic bound (now - 120) can never be reached
     * because the sender's newest minute is far behind it. */
    sender_t s2;
    sender_fill(&s2, 1000000, 30);
    s2.synthetic_latest = true;
    receiver_t r2;
    memset(&r2, 0, sizeof(r2));
    ctx_t x2 = {.sender = &s2, .receiver = &r2, .cursor = 0};
    history_sync_result_t res2;
    run_session(&x2, 0, &res2);
    CHECK(!res2.caught_up, "synthetic latest_utc reproduces the never-caught-up bug");
}

int main(void)
{
    scenario_clean_mirror();
    scenario_sleepy_link_resumes();
    scenario_clockless_sender_caught_up();

    printf("virtual day: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
