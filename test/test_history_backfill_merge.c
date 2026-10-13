/* Host test for power_history_merge(): the store-side half of the ESP-NOW
 * history backfill. The pure window/presence decision is covered by
 * test_history_backfill.c; this drives the real store (queue + writer task +
 * file) to prove a pulled record is validated, persisted and read back, and
 * that a corrupt record is rejected instead of poisoning the history.
 *
 * It uses the same plain-file fallback as test_reboot_persistence.c: the mount
 * point and history file are redirected under the build tmp dir by the runner.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "power_history.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);    \
        }                                                               \
    } while (0)

static uint32_t crc_record(const power_history_record_t *r)
{
    const uint8_t *b = (const uint8_t *)r;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < offsetof(power_history_record_t, checksum); ++i) {
        crc ^= b[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

static power_history_record_t make_record(uint32_t seq, int64_t start_utc, int32_t mw)
{
    power_history_record_t r;
    memset(&r, 0, sizeof(r));
    r.sequence = seq;
    r.interval_start_utc = start_utc;
    r.average_milliwatts = mw;
    r.sample_count = 60;
    r.format_version = POWER_HISTORY_VERSION;
    r.checksum = crc_record(&r);
    return r;
}

static void wait_for_count(uint32_t expected)
{
    power_history_status_t st;
    for (int i = 0; i < 300; ++i) {
        power_history_get_status(&st);
        if (st.record_count == expected) {
            return;
        }
        usleep(10000);
    }
}

/* Enqueue `n` minute records in one burst and wait until the writer has flushed
 * them. The writer flushes every 5 records, so bursts are multiples of 5. */
static void enqueue_batch(const int64_t *minutes, const int32_t *mw, size_t n,
                          uint32_t expected_count)
{
    for (size_t i = 0; i < n; ++i) {
        power_history_record_t r = make_record((uint32_t)(1000 + i), minutes[i], mw[i]);
        (void)power_history_enqueue(&r);
    }
    wait_for_count(expected_count);
}

static void test_read_after_and_has_minute(void)
{
    printf("test_read_after_and_has_minute\n");
    power_history_status_t st;
    power_history_get_status(&st);
    uint32_t before = st.record_count;

    /* Five distinct minutes past the earlier tests (which used 7000000000). */
    const int64_t base = 8000000000LL;
    int64_t minutes[5] = {base, base + 60, base + 120, base + 180, base + 240};
    int32_t mw[5] = {3000, 3001, 3002, 3003, 3004};
    enqueue_batch(minutes, mw, 5, before + 5);

    CHECK(power_history_has_minute(base), "oldest new minute is present");
    CHECK(power_history_has_minute(base + 240), "newest new minute is present");
    CHECK(!power_history_has_minute(base - 60), "minute before the block absent");
    CHECK(!power_history_has_minute(base + 300), "minute after the block absent");

    /* Page the block with a cursor just before it: minutes come back oldest
     * first, including base that a window read with from_utc=0 would miss. */
    power_history_record_t page[4];
    size_t count = 0;
    bool more = false;
    int64_t cursor = base - 60;
    int seen = 0;
    int64_t expect = base;
    while (true) {
        CHECK(power_history_read_after(cursor, 4, page, 4, &count, &st, &more) ==
                  ESP_OK,
              "read_after ok");
        for (size_t i = 0; i < count; ++i) {
            CHECK(page[i].interval_start_utc == expect,
                  "read_after page is time ordered from the oldest minute");
            expect += 60;
            ++seen;
        }
        if (count != 0) {
            cursor = page[count - 1].interval_start_utc;
        }
        if (!more) {
            break;
        }
    }
    CHECK(seen == 5, "read_after walks the whole block");

    /* From a zero cursor the oldest stored record is returned first (nothing is
     * skipped, which a window read with from_utc=0 would do). */
    CHECK(power_history_read_after(0, 4, page, 4, &count, &st, &more) == ESP_OK,
          "zero-cursor read ok");
    CHECK(count >= 1 && page[0].interval_start_utc < base,
          "zero cursor starts at the store's oldest minute");

    /* A cursor at the tail yields nothing. */
    CHECK(power_history_read_after(base + 240, 4, page, 4, &count, &st, &more) ==
              ESP_OK,
          "tail read ok");
    CHECK(count == 0 && !more, "nothing after the newest minute");
}

static void test_store_dedupe_by_minute(void)
{
    printf("test_store_dedupe_by_minute\n");
    const int64_t base = 8100000000LL;
    power_history_status_t st;
    power_history_get_status(&st);
    uint32_t before = st.record_count;

    int64_t minutes[5] = {base, base + 60, base + 120, base + 180, base + 240};
    int32_t mw[5] = {4000, 4001, 4002, 4003, 4004};
    enqueue_batch(minutes, mw, 5, before + 5);
    power_history_get_status(&st);
    CHECK(st.record_count == before + 5, "five new minutes stored");

    /* The same five minutes again with different values: the writer must drop
     * them so the store does not grow. A full batch of 5 forces the flush. */
    int32_t dup_mw[5] = {9999, 9999, 9999, 9999, 9999};
    enqueue_batch(minutes, dup_mw, 5, before + 5);
    usleep(300000);
    power_history_get_status(&st);
    CHECK(st.record_count == before + 5, "duplicate minutes were not stored again");

    /* The stored values are the originals, not the duplicates. */
    bool kept = false;
    power_history_record_t page[8];
    size_t count = 0;
    bool more = false;
    CHECK(power_history_read_after(base - 60, 8, page, 8, &count, &st, &more) == ESP_OK,
          "read_after ok");
    for (size_t i = 0; i < count; ++i) {
        if (page[i].interval_start_utc == base) {
            kept = page[i].average_milliwatts == 4000;
        }
    }
    CHECK(kept, "the original minute value is kept");
}

static void cleanup(void)
{
    remove(POWER_HISTORY_FILE);
    rmdir(POWER_HISTORY_MOUNT_PATH);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mkdir(POWER_HISTORY_MOUNT_PATH, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "could not create %s: %s\n",
                POWER_HISTORY_MOUNT_PATH, strerror(errno));
        return 2;
    }
    atexit(cleanup);

    printf("test_history_backfill_merge\n");
    if (power_history_init() != ESP_OK) {
        fprintf(stderr, "init failed\n");
        return 3;
    }

    const int64_t base = 7000000000LL;
    /* The receiver's own live minutes: base+0 .. base+4. */
    for (int i = 0; i < 5; ++i) {
        power_history_record_t r = make_record((uint32_t)(i + 1), base + i * 60,
                                               1000 + i);
        CHECK(power_history_enqueue(&r), "enqueue live minute");
    }
    wait_for_count(5);
    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.record_count == 5, "five live minutes stored");

    /* Pulled from the sender: five genuinely missing minutes (base+5..9) and one
     * corrupt record that must be rejected. Five fills one writer batch, so the
     * store flushes without waiting for the 60 s batch timeout. */
    power_history_record_t pulled[6];
    for (int i = 0; i < 5; ++i) {
        pulled[i] = make_record((uint32_t)(100 + i), base + (5 + i) * 60, 2000 + i);
    }
    pulled[5] = make_record(105, base + 10 * 60, 2005);
    pulled[5].checksum ^= 0xDEADBEEFu; /* corrupt: checksum mismatch */

    size_t merged = 0;
    CHECK(power_history_merge(pulled, 6, &merged) == ESP_OK, "merge returns OK");
    CHECK(merged == 5, "only the five valid records merge (corrupt skipped)");

    wait_for_count(10);
    power_history_get_status(&st);
    CHECK(st.record_count == 10, "store grew by exactly the merged minutes");

    /* The merged minutes are readable and carry the sender's values. */
    power_history_record_t readback[16];
    size_t count = 0;
    bool overflow = false;
    CHECK(power_history_read(0, 0, readback, 16, &count, &st, &overflow) == ESP_OK,
          "read back history");
    CHECK(count == 10, "ten minutes present");
    bool found5 = false;
    bool found9 = false;
    bool found_corrupt = false;
    for (size_t i = 0; i < count; ++i) {
        if (readback[i].interval_start_utc == base + 5 * 60 &&
            readback[i].average_milliwatts == 2000) {
            found5 = true;
        }
        if (readback[i].interval_start_utc == base + 9 * 60) {
            found9 = true;
        }
        if (readback[i].interval_start_utc == base + 10 * 60) {
            found_corrupt = true;
        }
    }
    CHECK(found5, "merged minute base+5 persisted with its value");
    CHECK(found9, "merged minute base+9 persisted");
    CHECK(!found_corrupt, "the corrupt record was not persisted");

    /* Sender side: the windowed page read the ESP-NOW `history` section uses.
     * Only minutes in [from, to) are returned, in time order, and `more` is set
     * while further in-window records follow. */
    CHECK(power_history_record_in_window(&pulled[0], base + 5 * 60, base + 6 * 60),
          "in-window record accepted");
    CHECK(!power_history_record_in_window(&pulled[0], base + 6 * 60, base + 7 * 60),
          "out-of-window record rejected");
    power_history_record_t corrupt = pulled[0];
    corrupt.checksum ^= 0x1u;
    CHECK(!power_history_record_in_window(&corrupt, 0, base + 100 * 60),
          "corrupt record rejected by the window filter");

    power_history_record_t page[8];
    size_t page_count = 0;
    bool more = false;
    CHECK(power_history_read_window(base + 2 * 60, base + 7 * 60, 0, 3, page, 8,
                                    &page_count, &st, &more) == ESP_OK,
          "window page read ok");
    CHECK(page_count == 3, "first page is capped at 3");
    CHECK(more, "more is set while further in-window records follow");
    CHECK(page[0].interval_start_utc == base + 2 * 60 &&
          page[2].interval_start_utc == base + 4 * 60, "first page in time order");

    uint32_t cursor = page[page_count - 1].sequence;
    CHECK(power_history_read_window(base + 2 * 60, base + 7 * 60, cursor, 3, page, 8,
                                    &page_count, &st, &more) == ESP_OK,
          "second window page read ok");
    CHECK(page_count == 2, "second page has the rest");
    CHECK(!more, "no more once the window end is reached");
    CHECK(page[0].interval_start_utc == base + 5 * 60 &&
          page[1].interval_start_utc == base + 6 * 60, "second page in time order");

    CHECK(power_history_read_window(base + 100 * 60, base + 101 * 60, 0, 3, page, 8,
                                    &page_count, &st, &more) == ESP_OK,
          "empty window read ok");
    CHECK(page_count == 0 && !more, "empty window returns nothing");

    /* Empty merge is a no-op, not an error. */
    merged = 123;
    CHECK(power_history_merge(NULL, 0, &merged) == ESP_ERR_INVALID_ARG,
          "NULL merge rejected");
    CHECK(merged == 0, "merge count reset on invalid input");

    /* Full-store sync primitives: has_minute() dedupe and read_after() paging
     * that reaches the oldest record with a zero cursor. */
    test_read_after_and_has_minute();

    /* The writer must also dedupe by minute, so a minute the live recorder and
     * the sync both enqueue is stored exactly once. */
    test_store_dedupe_by_minute();

    printf("  history-backfill-merge: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
