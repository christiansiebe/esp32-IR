/* Host tests for the receiver-side backfill merge bookkeeping
 * (main/history_backfill.c). Pure C, no ESP-IDF: the window/presence logic that
 * decides which sender minutes the receiver still misses. */
#include <stdio.h>
#include <string.h>

#include "history_backfill.h"

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

#define WINDOW_SECONDS (10 * 60)
#define BUCKETS (WINDOW_SECONDS / HISTORY_BACKFILL_BUCKET_SECONDS)

static void test_reset_validation(void)
{
    uint8_t storage[BUCKETS];
    history_backfill_t bf;
    CHECK(history_backfill_reset(&bf, 600000, 600000 + WINDOW_SECONDS, storage,
                                 sizeof(storage)), "valid window");
    CHECK(bf.bucket_count == BUCKETS, "bucket count");
    CHECK(history_backfill_missing_count(&bf) == BUCKETS, "all missing at start");
    CHECK(!history_backfill_reset(&bf, 100, 100, storage, sizeof(storage)),
          "empty window rejected");
    CHECK(!history_backfill_reset(&bf, 200, 100, storage, sizeof(storage)),
          "inverted window rejected");
    CHECK(!history_backfill_reset(&bf, 100, 100 + WINDOW_SECONDS, storage, 4),
          "storage too small rejected");
    CHECK(!history_backfill_reset(NULL, 100, 100 + WINDOW_SECONDS, storage,
                                  sizeof(storage)), "NULL out rejected");
}

static void test_index_and_window(void)
{
    uint8_t storage[BUCKETS];
    history_backfill_t bf;
    CHECK(history_backfill_reset(&bf, 600000, 600000 + WINDOW_SECONDS, storage,
                                 sizeof(storage)), "reset");
    size_t index = 999;
    CHECK(history_backfill_index(&bf, 600000, &index) && index == 0, "start -> 0");
    CHECK(history_backfill_index(&bf, 600000 + 60, &index) && index == 1, "next -> 1");
    CHECK(history_backfill_index(&bf, 600000 + WINDOW_SECONDS - 60, &index) &&
          index == BUCKETS - 1, "last -> n-1");
    CHECK(!history_backfill_index(&bf, 600000 - 60, &index), "before window rejected");
    CHECK(!history_backfill_index(&bf, 600000 + WINDOW_SECONDS, &index),
          "window end is exclusive");
}

static void test_alignment(void)
{
    /* A non-minute-aligned start is rounded down so bucket indices stay stable. */
    uint8_t storage[BUCKETS];
    history_backfill_t bf;
    CHECK(history_backfill_reset(&bf, 600037, 600037 + WINDOW_SECONDS, storage,
                                 sizeof(storage)), "unaligned reset");
    CHECK(bf.from_utc == 600000, "start rounded down to the minute");
    size_t index = 999;
    CHECK(history_backfill_index(&bf, 600000, &index) && index == 0,
          "rounded start maps to bucket 0");
}

static void test_merge_dedupe(void)
{
    /* Receiver already recorded minutes 0,1,2. The sender offers 1,2,3,4: only
     * the missing 3 and 4 may merge (the receiver's live data is never
     * overwritten). */
    uint8_t storage[BUCKETS];
    history_backfill_t bf;
    int64_t from = 600000;
    CHECK(history_backfill_reset(&bf, from, from + WINDOW_SECONDS, storage,
                                 sizeof(storage)), "reset");
    history_backfill_mark_present(&bf, from + 0 * 60);
    history_backfill_mark_present(&bf, from + 1 * 60);
    history_backfill_mark_present(&bf, from + 2 * 60);
    CHECK(history_backfill_is_present(&bf, from + 1 * 60), "present marked");
    CHECK(!history_backfill_should_merge(&bf, from + 1 * 60), "no merge for held minute");
    CHECK(history_backfill_should_merge(&bf, from + 3 * 60), "merge for missing minute");

    int merged = 0;
    int64_t offered[] = {from + 1 * 60, from + 2 * 60, from + 3 * 60, from + 4 * 60};
    for (size_t i = 0; i < sizeof(offered) / sizeof(offered[0]); ++i) {
        if (history_backfill_should_merge(&bf, offered[i])) {
            ++merged;
            history_backfill_mark_present(&bf, offered[i]);
        }
    }
    CHECK(merged == 2, "exactly the two missing minutes merge");
    CHECK(history_backfill_missing_count(&bf) == BUCKETS - 5, "missing count shrinks");

    /* A minute outside the window is never merged. */
    CHECK(!history_backfill_should_merge(&bf, from - 60), "before window never merges");
    CHECK(!history_backfill_should_merge(&bf, from + WINDOW_SECONDS),
          "at/after window end never merges");
}

static void test_missing_count_and_idempotence(void)
{
    uint8_t storage[BUCKETS];
    history_backfill_t bf;
    CHECK(history_backfill_reset(&bf, 0, WINDOW_SECONDS, storage, sizeof(storage)),
          "reset at epoch-ish base");
    uint32_t before = history_backfill_missing_count(&bf);
    CHECK(before == BUCKETS, "everything missing");
    history_backfill_mark_present(&bf, 0);
    history_backfill_mark_present(&bf, 0);
    CHECK(history_backfill_missing_count(&bf) == before - 1, "double mark is idempotent");
}

int main(void)
{
    test_reset_validation();
    test_index_and_window();
    test_alignment();
    test_merge_dedupe();
    test_missing_count_and_idempotence();
    printf("test_history_backfill: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
