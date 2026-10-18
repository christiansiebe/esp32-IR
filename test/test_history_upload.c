/* Host tests for the CSV-restore streaming accumulator (main/history_upload.c).
 *
 * The module is the pure core of the receiver's "restore from backup" upload.
 * It is exercised here without ESP-IDF so the tricky parts - partial lines split
 * across reads, the optional column header, sorted 64-row batches, the unsorted
 * hint, and the abort-on-reject path - are machine-checked. The actual SPIFFS
 * insert (handed to the store via the flush callback) is covered by
 * test_history_csv_import_store.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "history_upload.h"

/* 2024-01-01T00:00:00Z, the epoch second every 2024-01-01 timestamp resolves to. */
#define BASE_UTC 1704067200LL

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

/* A recording "store": keeps every flushed batch (sorted order and counts) and
 * can be told to reject a given batch, to drive the abort path. */
typedef struct {
    int64_t minutes[4096];
    size_t count;
    size_t batches;
    size_t reject_batch; /* 1-based; 0 = never reject */
    bool saw_sorted_batch;
} fake_store_t;

static bool flush_cb(void *ctx, power_history_record_t *records, size_t count,
                     size_t *imported_out, size_t *skipped_out)
{
    fake_store_t *store = (fake_store_t *)ctx;
    ++store->batches;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0 && records[i].interval_start_utc < records[i - 1].interval_start_utc) {
            store->saw_sorted_batch = false;
        }
        if (store->count < sizeof(store->minutes) / sizeof(store->minutes[0])) {
            store->minutes[store->count++] = records[i].interval_start_utc;
        }
    }
    if (store->reject_batch != 0 && store->batches == store->reject_batch) {
        *imported_out = 0;
        *skipped_out = count;
        return false;
    }
    *imported_out = count;
    *skipped_out = 0;
    return true;
}

static void make_row(char *buf, size_t n, int64_t minute, double watts)
{
    /* 2024-01-01T00:00:00Z + minute*60, formatted like the export. */
    int64_t secs = minute * 60;
    int days = (int)(secs / 86400);
    int rem = (int)(secs % 86400);
    (void)days; /* keep the offset simple: minute is relative to a base date */
    snprintf(buf, n, "1,2024-01-01T%02d:%02d:%02dZ,%.3f,1,none\r\n",
             rem / 3600, (rem % 3600) / 60, rem % 60, watts);
}

static void test_basic_parse(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    const char *body =
        "sequence,interval_start_ms,average_w,sample_count,reason\r\n"
        "1,2024-01-01T00:00:00Z,100.000,1,none\r\n"
        "2,2024-01-01T00:01:00Z,110.000,1,none\r\n"
        "3,2024-01-01T00:02:00Z,120.000,1,none\r\n";
    history_upload_feed(&up, body, strlen(body));
    history_upload_finish(&up);

    CHECK(up.parsed == 3, "three data rows parsed");
    CHECK(up.imported == 3, "three rows imported");
    CHECK(up.malformed == 0, "header not counted malformed");
    CHECK(up.duplicates == 0, "no duplicates");
    CHECK(!up.unsorted, "ascending input not flagged unsorted");
    CHECK(store.count == 3, "store received three records");
    CHECK(store.minutes[0] == BASE_UTC && store.minutes[2] == BASE_UTC + 120,
          "minutes forwarded as epoch seconds");
}

static void test_partial_lines_across_reads(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    const char *body = "1,2024-01-01T00:00:00Z,1.000,1,none\n"
                       "2,2024-01-01T00:01:00Z,2.000,1,none\n";
    /* Feed one byte at a time: the line buffer must reassemble every row. */
    for (size_t i = 0; i < strlen(body); ++i) {
        history_upload_feed(&up, body + i, 1);
    }
    history_upload_finish(&up);

    CHECK(up.parsed == 2, "byte-wise feed parses both rows");
    CHECK(store.count == 2, "both rows reached the store");
}

static void test_trailing_line_without_newline(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    const char *body = "1,2024-01-01T00:00:00Z,1.000,1,none\n"
                       "2,2024-01-01T00:01:00Z,2.000,1,none"; /* no final \n */
    history_upload_feed(&up, body, strlen(body));
    history_upload_finish(&up);

    CHECK(up.parsed == 2, "trailing line without newline is flushed");
    CHECK(store.count == 2, "trailing row reached the store");
}

static void test_unsorted_detected(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    const char *body = "1,2024-01-01T00:05:00Z,1.000,1,none\n"
                       "2,2024-01-01T00:00:00Z,2.000,1,none\n";
    history_upload_feed(&up, body, strlen(body));
    history_upload_finish(&up);

    CHECK(up.unsorted, "out-of-order input flagged");
    CHECK(up.parsed == 2, "both rows still parsed");
    CHECK(store.saw_sorted_batch, "batch handed to the store is sorted");
    CHECK(store.minutes[0] == BASE_UTC && store.minutes[1] == BASE_UTC + 300,
          "store sees ascending order after the per-batch sort");
}

static void test_batch_boundary(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    /* HISTORY_UPLOAD_BATCH rows plus a few more: the first batch must flush at
     * exactly the boundary and the tail at finish. */
    char row[64];
    for (uint32_t i = 0; i < HISTORY_UPLOAD_BATCH + 5u; ++i) {
        make_row(row, sizeof(row), (int64_t)i, 100.0);
        history_upload_feed(&up, row, strlen(row));
    }
    CHECK(store.batches == 1, "one full batch flushed mid-stream");
    history_upload_finish(&up);
    CHECK(store.batches == 2, "tail flushed at finish");
    CHECK(up.imported == HISTORY_UPLOAD_BATCH + 5u, "all rows imported");
}

static void test_malformed_and_blank(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    const char *body =
        "sequence,interval_start_ms,average_w,sample_count,reason\n"
        "\n"
        "   \r\n"
        "not-a-row\n"
        "1,2024-01-01T00:00:00Z,1.000,1,none\n";
    history_upload_feed(&up, body, strlen(body));
    history_upload_finish(&up);

    CHECK(up.parsed == 1, "one valid row parsed");
    CHECK(up.malformed == 1, "the junk row counted malformed, blanks not");
}

static void test_pre_2024_row_rejected(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    /* A device that never had a wall clock exports a 1970 stamp; it must be
     * dropped as malformed, not imported (it would sort before every minute). */
    const char *body =
        "sequence,interval_start_ms,average_w,sample_count,reason\n"
        "1,1970-01-01T00:00:00Z,1.000,1,none\n"
        "2,2024-01-01T00:00:00Z,2.000,1,none\n";
    history_upload_feed(&up, body, strlen(body));
    history_upload_finish(&up);

    CHECK(up.parsed == 1, "only the post-2024 row parsed");
    CHECK(up.malformed == 1, "the 1970 row counted malformed");
    CHECK(store.count == 1 && store.minutes[0] == BASE_UTC, "only the valid row stored");
}

static void test_abort_on_reject(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    store.reject_batch = 1; /* reject the very first flush */
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    char row[64];
    for (uint32_t i = 0; i < HISTORY_UPLOAD_BATCH; ++i) {
        make_row(row, sizeof(row), (int64_t)i, 100.0);
        history_upload_feed(&up, row, strlen(row));
    }
    CHECK(up.aborted, "a rejected batch aborts the upload");
    /* Further input is ignored once aborted. */
    make_row(row, sizeof(row), 999, 100.0);
    history_upload_feed(&up, row, strlen(row));
    history_upload_finish(&up);
    CHECK(store.count == HISTORY_UPLOAD_BATCH, "no rows accepted after the abort");
}

static void test_long_line_dropped(void)
{
    fake_store_t store = {0};
    store.saw_sorted_batch = true;
    history_upload_t up;
    history_upload_init(&up, flush_cb, &store);

    /* A line longer than the buffer is counted malformed, never overflows. */
    char big[HISTORY_UPLOAD_LINE_MAX + 32];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\n';
    history_upload_feed(&up, big, sizeof(big));
    history_upload_finish(&up);
    CHECK(up.malformed == 1, "over-long line counted malformed");
    CHECK(up.parsed == 0, "over-long line not parsed");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_basic_parse();
    test_partial_lines_across_reads();
    test_trailing_line_without_newline();
    test_unsorted_detected();
    test_batch_boundary();
    test_malformed_and_blank();
    test_pre_2024_row_rejected();
    test_abort_on_reject();
    test_long_line_dropped();

    printf("history_upload: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
