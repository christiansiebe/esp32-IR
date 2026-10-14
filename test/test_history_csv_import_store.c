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
    test_import_arg_guards();

    printf("history_csv_import_store: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
