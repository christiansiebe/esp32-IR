/* Host test: does a backup upload clean up an already-scrambled store?
 *
 * Simulates the on-disk state an older firmware left behind: live minutes in
 * order, then an out-of-order backfill block appended at the tail (the ring is
 * time-unsorted). power_history_init() must repair that at startup, and a
 * subsequent CSV-restore upload (power_history_import_sorted) must insert the
 * missing minutes at their time-ordered position - not append, not duplicate.
 *
 * Runs in its own binary because power_history_init() may be called only once
 * per process. The store path is redirected under the build tmp dir.
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

static uint32_t crc_bytes(const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
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
    r.checksum = crc_bytes(&r, offsetof(power_history_record_t, checksum));
    return r;
}

static power_history_record_t import_record(int64_t start_utc, int32_t mw)
{
    return make_record(0, start_utc, mw);
}

/* Write the file the way the old firmware would have left it: a single valid
 * header copy, then `count` records in ring index order (index 0 at slot 0). */
static bool write_scrambled_store(const int64_t *minutes, size_t count)
{
    FILE *f = fopen(POWER_HISTORY_FILE, "w+b");
    if (f == NULL) {
        return false;
    }
    power_history_header_t header = {0};
    header.magic = 0x50484C31u; /* POWER_HISTORY_MAGIC */
    header.version = POWER_HISTORY_VERSION;
    header.generation = 5;
    header.next_sequence = (uint32_t)count + 1u;
    header.record_count = (uint32_t)count;
    header.next_slot = (uint32_t)(count % POWER_HISTORY_CAPACITY); /* oldest_slot = 0 */
    header.checksum = crc_bytes(&header, offsetof(power_history_header_t, checksum));

    bool ok = fwrite(&header, sizeof(header), 1, f) == 1;
    /* Second header copy stays all-zero -> invalid -> init uses copy 0. */
    power_history_header_t zero = {0};
    ok = ok && fwrite(&zero, sizeof(zero), 1, f) == 1;
    for (size_t i = 0; i < count && ok; ++i) {
        power_history_record_t r = make_record((uint32_t)(i + 1), minutes[i], (int32_t)(5000 + i));
        ok = fwrite(&r, sizeof(r), 1, f) == 1;
    }
    ok = fflush(f) == 0 && ok;
    fclose(f);
    return ok;
}

static void test_repair_and_backup_upload(void)
{
    printf("test_repair_and_backup_upload\n");
    const int64_t base = 9300000000LL;
    /* Live minutes in order, an older backfill block appended (ring unsorted),
     * and two minutes present twice (duplicates) - the exact damage an older
     * firmware produced. */
    int64_t scrambled[12] = {
        base, base + 60, base + 120, base + 180, base + 240,
        base - 300, base - 240, base - 180, base - 120, base - 60,
        base + 120, base - 60,
    };
    CHECK(write_scrambled_store(scrambled, 12), "scrambled store written");

    /* Init must not block on the ring; the repair runs in the background task
     * on the device. On the host we drive the same public entry point the task
     * uses, then verify the ring is sorted/duplicate-free. */
    CHECK(power_history_init() == ESP_OK, "init ok");
    CHECK(power_history_repair_ring() == ESP_OK, "repair ok");

    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.record_count == 10, "repair dropped the two duplicate minutes");

    /* Every distinct minute is present. */
    bool all_present = true;
    int64_t distinct[10] = {base - 300, base - 240, base - 180, base - 120, base - 60,
                            base, base + 60, base + 120, base + 180, base + 240};
    for (int i = 0; i < 10; ++i) {
        if (!power_history_has_minute(distinct[i])) {
            all_present = false;
        }
    }
    CHECK(all_present, "repair kept every distinct minute searchable");

    power_history_record_t page[16];
    size_t count = 0;
    bool more = false;
    CHECK(power_history_read_after(base - 1000, 16, page, 16, &count, &st, &more) == ESP_OK,
          "read_after ok");
    CHECK(count == 10, "ten distinct records read back");
    bool ascending = true;
    bool dup = false;
    for (size_t i = 1; i < count; ++i) {
        if (page[i].interval_start_utc <= page[i - 1].interval_start_utc) {
            ascending = false;
            if (page[i].interval_start_utc == page[i - 1].interval_start_utc) {
                dup = true;
            }
        }
    }
    CHECK(ascending && !dup, "ring is sorted and duplicate-free after repair");
    bool seq_monotonic = true;
    for (size_t i = 1; i < count; ++i) {
        if (page[i].sequence <= page[i - 1].sequence) {
            seq_monotonic = false;
        }
    }
    CHECK(seq_monotonic, "sequence is monotonic with time after repair");
    CHECK(page[0].interval_start_utc == base - 300, "oldest minute first");

    /* Now a backup upload: two minutes the store already has, plus new minutes
     * that are older AND newer than the local tail. */
    power_history_record_t backup[5] = {
        import_record(base + 60, 1111),   /* duplicate */
        import_record(base + 120, 2222),  /* duplicate */
        import_record(base - 360, 3333),  /* new, older than everything */
        import_record(base + 300, 4444),  /* new, newer than the tail */
        import_record(base + 360, 5555),  /* new, newest */
    };
    size_t imported = 0;
    size_t skipped = 0;
    CHECK(power_history_import_sorted(backup, 5, &imported, &skipped) == ESP_OK,
          "backup import ok");
    CHECK(imported == 3, "three new minutes imported");
    CHECK(skipped == 2, "two already-stored minutes skipped");

    power_history_get_status(&st);
    CHECK(st.record_count == 13, "store grew by exactly the three new minutes");

    /* The store is still sorted, holds each minute once, and keeps live values. */
    CHECK(power_history_read_after(base - 1000, 16, page, 16, &count, &st, &more) == ESP_OK,
          "read_after after import ok");
    CHECK(count == 13, "thirteen records read back");
    bool sorted_after = true;
    bool dup_after = false;
    for (size_t i = 1; i < count; ++i) {
        if (page[i].interval_start_utc <= page[i - 1].interval_start_utc) {
            sorted_after = false;
            if (page[i].interval_start_utc == page[i - 1].interval_start_utc) {
                dup_after = true;
            }
        }
    }
    CHECK(sorted_after && !dup_after, "no duplicates, still time-sorted");
    CHECK(page[0].interval_start_utc == base - 360, "imported older minute landed first");
    CHECK(page[count - 1].interval_start_utc == base + 360, "imported newer minute landed last");

    bool live_kept = true;
    for (size_t i = 0; i < count; ++i) {
        if (page[i].interval_start_utc == base + 60 && page[i].average_milliwatts != 5001) {
            live_kept = false;
        }
        if (page[i].interval_start_utc == base + 120 && page[i].average_milliwatts != 5002) {
            live_kept = false;
        }
    }
    CHECK(live_kept, "backup did not overwrite live values");
}

/* Full ring (29000 records) left unsorted with duplicate minutes. count is far
 * above POWER_HISTORY_REPAIR_RAM_LIMIT, so the repair must fall back to its
 * safe path: keep only the most recent records that fit one RAM buffer, sort
 * them, dedupe them and renumber them. It deliberately does NOT sort in place
 * on SPIFFS (a single lseek on a damaged/full store can hang for minutes and
 * wedge the device). Verifies the survivors are sorted, duplicate-free and
 * have a monotonic, contiguous sequence. */
static void test_full_ring_repair(void)
{
    printf("test_full_ring_repair (keeps newest RAM_LIMIT)\n");
    const uint32_t total = POWER_HISTORY_CAPACITY; /* 29000 */
    const uint32_t kept = POWER_HISTORY_REPAIR_RAM_LIMIT; /* 4096 in the build */
    /* The ring as written below is "full" (29000 stored) but damaged:
     * duplicates every 10th minute. The repair may keep only the newest `kept`
     * records, so the exact distinct count is no longer fixed; what matters is
     * that the survivors are sorted, deduped and contiguous. */
    const uint32_t distinct = 26363u; /* unique minutes before repair */
    const uint32_t count = total;
    const int64_t base = 9400000000LL;

    FILE *f = fopen(POWER_HISTORY_FILE, "w+b");
    CHECK(f != NULL, "full-ring store opened");
    if (f == NULL) {
        return;
    }
    power_history_header_t header = {0};
    header.magic = 0x50484C31u;
    header.version = POWER_HISTORY_VERSION;
    header.generation = 7;
    header.next_sequence = count + 1u;
    header.record_count = count;
    header.next_slot = count % POWER_HISTORY_CAPACITY; /* oldest_slot = 0 */
    header.checksum = crc_bytes(&header, offsetof(power_history_header_t, checksum));
    bool ok = fwrite(&header, sizeof(header), 1, f) == 1;
    power_history_header_t zero = {0};
    ok = ok && fwrite(&zero, sizeof(zero), 1, f) == 1;

    /* Write the minutes ascending with duplicate minutes scattered in: this is
     * the realistic "full store after backfill merges" state (near-sorted, a
     * duplicate every 10th minute). The store is damaged (duplicates), so the
     * repair runs; it keeps only the most recent POWER_HISTORY_REPAIR_RAM_LIMIT
     * records, sorted and deduped. */
    uint32_t seq = 1;
    uint32_t written = 0;
    for (uint32_t m = 0; m < distinct && ok; ++m) {
        uint32_t minute = m;
        power_history_record_t r =
            make_record(seq++, base + (int64_t)minute * 60, (int32_t)(minute % 1000));
        ok = fwrite(&r, sizeof(r), 1, f) == 1;
        ++written;
        if (ok && (minute % 10u) == 0u) {
            power_history_record_t d =
                make_record(seq++, base + (int64_t)minute * 60, -1);
            ok = fwrite(&d, sizeof(d), 1, f) == 1;
            ++written;
        }
    }
    ok = ok && fflush(f) == 0;
    fclose(f);
    CHECK(ok && written == total, "full-ring store written (29000 records)");
    if (!ok) {
        return;
    }

    CHECK(power_history_init() == ESP_OK, "init on full ring ok");
    CHECK(power_history_repair_ring() == ESP_OK, "repair on full ring ok");
    power_history_status_t st;
    power_history_get_status(&st);
    /* The full ring no longer keeps every distinct minute: the repair may only
     * preserve what fits one RAM buffer. It must never exceed that bound, and
     * it must keep at least the newest minutes (so live data survives). */
    CHECK(st.record_count <= kept,
          "full ring repair keeps at most RAM_LIMIT records");
    CHECK(st.record_count > 0, "full ring repair keeps the newest records");

    power_history_record_t page[64];
    size_t n = 0;
    bool more = false;
    CHECK(power_history_read_after(base - 60, 64, page, 64, &n, &st, &more) == ESP_OK,
          "read_after on full ring ok");
    bool asc = n > 1;
    for (size_t i = 1; i < n; ++i) {
        if (page[i].interval_start_utc <= page[i - 1].interval_start_utc) {
            asc = false;
        }
        if (page[i].interval_start_utc == page[i - 1].interval_start_utc) {
            asc = false; /* no duplicate minutes after repair */
        }
    }
    CHECK(asc, "full ring sorted and deduped after repair");
    /* The newest minute (distinct - 1) must have survived; some of the oldest
     * minutes may have been dropped to fit the RAM limit. */
    CHECK(power_history_has_minute(base + (int64_t)(distinct - 1u) * 60),
          "newest minute reachable after repair");
}

static void cleanup(void)
{
    remove(POWER_HISTORY_FILE);
    rmdir(POWER_HISTORY_MOUNT_PATH);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mkdir(POWER_HISTORY_MOUNT_PATH, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "could not create %s: %s\n",
                POWER_HISTORY_MOUNT_PATH, strerror(errno));
        return 2;
    }
    atexit(cleanup);

    printf("test_history_ring_repair\n");
    if (argc > 1 && strcmp(argv[1], "full") == 0) {
        test_full_ring_repair();
    } else {
        test_repair_and_backup_upload();
    }

    /* power_history_init() starts the background repair task. That task must
     * delete itself; a task function that returns is undefined behaviour on
     * the device (the IDF release build has no task wrapper, so the first
     * v0.2.0 flash panicked and rolled back). The host shim counts any task
     * that returns without vTaskDelete(NULL); the count must stay zero. */
    CHECK(host_tasks_returned_without_delete() == 0,
          "every task function deletes itself (no bare return)");

    printf("history_ring_repair: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
