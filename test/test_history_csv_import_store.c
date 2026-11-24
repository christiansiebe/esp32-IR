/* Host test for power_history_import_sorted(): the store-side half of the CSV
 * restore upload. Drives the real store (file + header) to prove a backup import
 * appends new minutes, dedupes existing ones (keeping the original value) and
 * leaves the store readable. Same plain-file fallback as the other store tests:
 * the mount point and history file are redirected under the build tmp dir.
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

/* An imported record carries no sequence; the store assigns it. */
static power_history_record_t import_record(int64_t start_utc, int32_t mw)
{
    return make_record(0, start_utc, mw);
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

static void enqueue_batch(const int64_t *minutes, const int32_t *mw, size_t n,
                          uint32_t expected_count)
{
    for (size_t i = 0; i < n; ++i) {
        power_history_record_t r = make_record((uint32_t)(1000 + i), minutes[i], mw[i]);
        (void)power_history_enqueue(&r);
    }
    wait_for_count(expected_count);
}

static void test_import_extends_and_dedupes(void)
{
    printf("test_import_extends_and_dedupes\n");
    const int64_t base = 9000000000LL;
    power_history_status_t st;
    power_history_get_status(&st);
    uint32_t before = st.record_count;

    /* Receiver's own live minutes base+0 .. base+4. */
    int64_t minutes[5] = {base, base + 60, base + 120, base + 180, base + 240};
    int32_t mw[5] = {5000, 5001, 5002, 5003, 5004};
    enqueue_batch(minutes, mw, 5, before + 5);

    /* A backup that repeats two live minutes and adds four new ones. */
    power_history_record_t imported[6] = {
        import_record(base, 9999),          /* duplicate of a live minute */
        import_record(base + 60, 9999),     /* duplicate of a live minute */
        import_record(base + 300, 6000),    /* new */
        import_record(base + 360, 6001),    /* new */
        import_record(base + 420, 6002),    /* new */
        import_record(base + 480, 6003),    /* new */
    };
    size_t imported_n = 0;
    size_t skipped = 0;
    CHECK(power_history_import_sorted(imported, 6, &imported_n, &skipped) == ESP_OK,
          "import returns ok");
    CHECK(imported_n == 4, "four new minutes imported");
    CHECK(skipped == 2, "two duplicate minutes skipped");

    power_history_get_status(&st);
    CHECK(st.record_count == before + 9, "store grew by the four new minutes");

    CHECK(power_history_has_minute(base + 300), "new minute present");
    CHECK(power_history_has_minute(base + 480), "last new minute present");
    CHECK(!power_history_has_minute(base + 540), "minute after the import absent");

    /* The live value is not overwritten by the backup. */
    bool kept = false;
    power_history_record_t page[8];
    size_t count = 0;
    bool more = false;
    CHECK(power_history_read_after(base - 60, 8, page, 8, &count, &st, &more) == ESP_OK,
          "read_after ok");
    for (size_t i = 0; i < count; ++i) {
        if (page[i].interval_start_utc == base) {
            kept = page[i].average_milliwatts == 5000;
        }
    }
    CHECK(kept, "live minute value is kept over the backup");
}

static void test_import_forward_order(void)
{
    printf("test_import_forward_order\n");
    const int64_t base = 9100000000LL;
    power_history_status_t st;

    power_history_record_t imported[3] = {
        import_record(base, 7000),
        import_record(base + 60, 7001),
        import_record(base + 120, 7002),
    };
    size_t imported_n = 0;
    size_t skipped = 0;
    CHECK(power_history_import_sorted(imported, 3, &imported_n, &skipped) == ESP_OK,
          "import into empty region ok");
    CHECK(imported_n == 3 && skipped == 0, "three minutes imported");

    /* A forward-extending import keeps the ring time-ordered. */
    power_history_record_t page[4];
    size_t count = 0;
    bool more = false;
    CHECK(power_history_read_after(base - 60, 4, page, 4, &count, &st, &more) == ESP_OK,
          "read_after ok");
    CHECK(count == 3, "three records read back");
    CHECK(page[0].interval_start_utc == base, "oldest first");
    CHECK(page[1].interval_start_utc == base + 60, "ascending");
    CHECK(page[2].interval_start_utc == base + 120, "newest last");
}

static void test_import_idempotent(void)
{
    printf("test_import_idempotent\n");
    const int64_t base = 9200000000LL;
    power_history_status_t st;
    power_history_get_status(&st);
    uint32_t before = st.record_count;

    power_history_record_t imported[2] = {
        import_record(base, 8000),
        import_record(base + 60, 8001),
    };
    size_t imported_n = 0;
    size_t skipped = 0;
    CHECK(power_history_import_sorted(imported, 2, &imported_n, &skipped) == ESP_OK,
          "first import ok");
    CHECK(imported_n == 2, "two imported");

    CHECK(power_history_import_sorted(imported, 2, &imported_n, &skipped) == ESP_OK,
          "second import ok");
    CHECK(imported_n == 0 && skipped == 2, "re-import is a no-op");

    power_history_get_status(&st);
    CHECK(st.record_count == before + 2, "count unchanged by the re-import");
}

/* A record stamped before the valid-clock bound (0 / 1970 from a device that
 * never had a wall clock) must be rejected at ingestion: it would sort before
 * every stored minute and break the time-ordered ring's dedupe and paging. */
static void test_import_rejects_pre_2024(void)
{
    printf("test_import_rejects_pre_2024\n");
    const int64_t base = 9400000000LL;
    power_history_status_t st;
    power_history_get_status(&st);
    uint32_t before = st.record_count;

    power_history_record_t imported[3] = {
        import_record(0, 1111),               /* bucket_start == 0 */
        import_record(1700000000LL, 2222),    /* 2023-11: still pre-2024 */
        import_record(base, 3333),            /* valid */
    };
    size_t imported_n = 0;
    size_t skipped = 0;
    CHECK(power_history_import_sorted(imported, 3, &imported_n, &skipped) == ESP_OK,
          "import ok");
    CHECK(imported_n == 1, "only the valid minute imported");
    CHECK(skipped == 2, "the two pre-2024 minutes skipped");

    power_history_get_status(&st);
    CHECK(st.record_count == before + 1, "store grew by one minute");
    CHECK(!power_history_has_minute(0), "the 0 minute was not stored");
    CHECK(!power_history_has_minute(1700000000LL), "the 2023 minute was not stored");
    CHECK(power_history_has_minute(base), "the valid minute is present");
}

/* `storage_ready` must not claim persistence when the store is only in RAM.
 * With a real file open (as here), status.persistent is true. */
static void test_status_persistent(void)
{
    printf("test_status_persistent\n");
    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.ready, "store ready");
    CHECK(st.persistent, "persistent flag set when the file is open");
}

/* While a CSV restore is streaming, live/backfill producers must be quiesced:
 * enqueue() refuses new minutes so the restore can be the only writer. The
 * restore itself (import_sorted) still works, and once the flag is cleared the
 * live path resumes. */
static void test_restore_quiesces_live(void)
{
    printf("test_restore_quiesces_live\n");
    const int64_t base = 9600000000LL;
    power_history_status_t st;

    power_history_get_status(&st);
    uint32_t before = st.record_count;

    power_history_set_restore_active(true);
    CHECK(power_history_restore_active(), "restore flag reads back true");

    /* import_sorted is the restore path and must still write. */
    power_history_record_t imported[2] = {
        import_record(base, 9000),
        import_record(base + 60, 9001),
    };
    size_t imported_n = 0;
    size_t skipped = 0;
    CHECK(power_history_import_sorted(imported, 2, &imported_n, &skipped) == ESP_OK,
          "import works while restore active");
    CHECK(imported_n == 2, "two minutes imported during restore");
    CHECK(power_history_has_minute(base), "imported minute present");

    /* A live/backfill minute offered via enqueue is refused, so it must not
     * appear in the ring while the flag is set. */
    power_history_record_t live = make_record(9999, base + 120, 111);
    CHECK(!power_history_enqueue(&live), "enqueue refused while restore active");
    usleep(50000);
    power_history_get_status(&st);
    CHECK(!power_history_has_minute(base + 120), "refused minute not stored");

    /* Clearing the flag re-enables the live path. A full batch (the worker
     * gathers POWER_HISTORY_BATCH_RECORDS before flushing) is needed so the
     * write happens at once instead of waiting out the batch timeout. */
    power_history_set_restore_active(false);
    CHECK(!power_history_restore_active(), "restore flag cleared");
    power_history_record_t live_batch[5];
    for (size_t i = 0; i < 5; ++i) {
        live_batch[i] = make_record((uint32_t)(9999 + i), base + 120 + (int64_t)i * 60,
                                    111 + (int32_t)i);
        CHECK(power_history_enqueue(&live_batch[i]), "enqueue accepted after restore");
    }
    wait_for_count(before + 2 + 5); /* the two imported plus the live batch */
    CHECK(power_history_has_minute(base + 120), "live minute stored after resume");
    CHECK(power_history_has_minute(base + 240), "last live minute stored");
}

static void test_import_arg_guards(void)
{
    printf("test_import_arg_guards\n");
    size_t imported_n = 0;
    size_t skipped = 0;
    CHECK(power_history_import_sorted(NULL, 1, &imported_n, &skipped) ==
              ESP_ERR_INVALID_ARG,
          "null records rejected");
    power_history_record_t one = import_record(9300000000LL, 1);
    CHECK(power_history_import_sorted(&one, 0, &imported_n, &skipped) ==
              ESP_ERR_INVALID_ARG,
          "zero count rejected");
}

/* The watchdog-fairness constants must stay well under the TWDT 120 s timeout
 * (a value at or above it would re-introduce the very stall that rebooted the
 * device during the restore), and the import-feed stride must be non-zero. */
static void test_wdt_fairness_constants(void)
{
    printf("test_wdt_fairness_constants\n");
    CHECK(POWER_HISTORY_WRITE_LOCK_TIMEOUT_MS > 0,
          "write-lock timeout is positive");
    CHECK(POWER_HISTORY_WRITE_LOCK_TIMEOUT_MS < 120000,
          "write-lock timeout stays under the TWDT 120 s timeout");
    CHECK(POWER_HISTORY_IMPORT_FEED_EVERY > 0,
          "import-feed stride is positive");
}

/* A restore that inserts minutes deep below the tail of an already-full ring
 * performs a splice (shift_up_renumber) for every such minute. The import must
 * stay correct (time-ordered, all minutes present, full ring readable) under
 * that splice load, so a long restore does not corrupt the store while the
 * watchdog-fairness changes keep the worker alive. */
static void test_import_splice_pressure(void)
{
    printf("test_import_splice_pressure\n");
    const int64_t base = 9700000000LL;
    power_history_status_t st;

    /* Pre-fill a region with minutes base+N*60 so an import of the same base
     * minutes (older than the tail) forces an in-place splice each time. */
    power_history_record_t existing[100];
    for (size_t i = 0; i < 100; ++i) {
        existing[i] = import_record(base + (int64_t)i * 60, (int32_t)(10000 + i));
    }
    size_t imported_n = 0, skipped = 0;
    CHECK(power_history_import_sorted(existing, 100, &imported_n, &skipped) == ESP_OK,
          "pre-fill import ok");
    CHECK(imported_n == 100 && skipped == 0, "all pre-fill minutes imported");

    /* Import the same minutes again but stamped as "older" backups. They are
     * duplicates (same minute), so they must be skipped, not spliced - and a
     * second region of brand-new lower minutes (base-M) must splice in. */
    power_history_record_t splice_in[64];
    for (size_t i = 0; i < 64; ++i) {
        splice_in[i] = import_record(base - (int64_t)(64 - i) * 60,
                                     (int32_t)(20000 + i));
    }
    CHECK(power_history_import_sorted(splice_in, 64, &imported_n, &skipped) == ESP_OK,
          "splice import ok");
    CHECK(imported_n == 64 && skipped == 0, "all splice minutes imported");

    /* Every minute - the pre-fill and the spliced-in block - must be present,
     * and the ring must still read in strict time order. */
    CHECK(power_history_has_minute(base - 60), "first spliced minute present");
    CHECK(power_history_has_minute(base), "pre-fill start present");
    CHECK(power_history_has_minute(base + 99 * 60), "pre-fill end present");
    CHECK(power_history_has_minute(base - 64 * 60), "oldest spliced minute present");

    power_history_record_t page[256];
    size_t count = 0;
    bool more = false;
    CHECK(power_history_read_after(base - 65 * 60, 256, page, 256, &count, &st, &more) ==
              ESP_OK,
          "read after splice ok");
    int64_t last_utc = 0;
    bool first = true;
    bool ascending = true;
    for (size_t i = 0; i < count; ++i) {
        if (page[i].interval_start_utc == 0) {
            continue;
        }
        if (!first && page[i].interval_start_utc <= last_utc) {
            ascending = false;
        }
        last_utc = page[i].interval_start_utc;
        first = false;
    }
    CHECK(ascending, "ring stays time-ordered after splice pressure");
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

    printf("test_history_csv_import_store\n");
    if (power_history_init() != ESP_OK) {
        fprintf(stderr, "init failed\n");
        return 3;
    }

    test_import_extends_and_dedupes();
    test_import_forward_order();
    test_import_idempotent();
    test_import_rejects_pre_2024();
    test_status_persistent();
    test_restore_quiesces_live();
    test_wdt_fairness_constants();
    test_import_splice_pressure();
    test_import_arg_guards();

    printf("history_csv_import_store: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
