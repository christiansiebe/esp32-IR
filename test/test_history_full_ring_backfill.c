/* Host regression test: an OLDER backfill minute must not evict the NEWEST
 * minute from a FULL history ring.
 *
 * The live recorder appends the newest minute at the tail, so write_records()
 * usually takes the cheap "at == count" path. A backfill merge carries an OLDER
 * minute and is spliced into its time-ordered position, shifting the newer
 * records up by one slot. On a full ring that shift evicts the topmost record -
 * which, for an insert anywhere but the tail, is the NEWEST minute. The old code
 * then also wrote the incoming older record into the just-freed slot, so the
 * newest minute was lost AND the ring was left time-unsorted (the new older
 * record sat at the newest position), breaking the binary-search dedupe.
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

/* Write a FULL, time-sorted ring the way the live recorder would leave it:
 * oldest at index 0 (slot 0), newest at the tail, next_slot wraps to 0. */
static bool write_full_sorted_store(const int64_t *minutes, uint32_t count)
{
    FILE *f = fopen(POWER_HISTORY_FILE, "w+b");
    if (f == NULL) {
        return false;
    }
    power_history_header_t header = {0};
    header.magic = 0x50484C31u; /* POWER_HISTORY_MAGIC */
    header.version = POWER_HISTORY_VERSION;
    header.generation = 9;
    header.next_sequence = count + 1u;
    header.record_count = count;
    header.next_slot = count % POWER_HISTORY_CAPACITY; /* 0 when full */
    header.checksum = crc_bytes(&header, offsetof(power_history_header_t, checksum));
    bool ok = fwrite(&header, sizeof(header), 1, f) == 1;
    /* Second header copy stays all-zero -> invalid -> init uses copy 0. */
    power_history_header_t zero = {0};
    ok = ok && fwrite(&zero, sizeof(zero), 1, f) == 1;
    for (uint32_t i = 0; i < count && ok; ++i) {
        power_history_record_t r = make_record(i + 1u, minutes[i], (int32_t)(i % 1000));
        ok = fwrite(&r, sizeof(r), 1, f) == 1;
    }
    ok = ok && fflush(f) == 0;
    fclose(f);
    return ok;
}

static void wait_for_quiet(void)
{
    /* The writer flushes a 5-record batch promptly; give it time to finish the
     * (possibly long) in-place insert on the full ring. */
    for (int i = 0; i < 600; ++i) {
        usleep(10000);
    }
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

    printf("test_history_full_ring_backfill\n");

    const uint32_t total = POWER_HISTORY_CAPACITY; /* 29000 */
    const int64_t base = 9600000000LL;
    int64_t *minutes = malloc((size_t)total * sizeof(*minutes));
    if (minutes == NULL) {
        fprintf(stderr, "no memory for the minute list\n");
        return 2;
    }
    for (uint32_t i = 0; i < total; ++i) {
        minutes[i] = base + (int64_t)i * 60;
    }
    const int64_t oldest = minutes[0];
    const int64_t newest = minutes[total - 1u];

    CHECK(write_full_sorted_store(minutes, total), "full sorted ring written");
    free(minutes);

    CHECK(power_history_init() == ESP_OK, "init on full ring ok");
    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.record_count == total, "full ring loaded with 29000 records");

    /* A backfill merge of minutes OLDER than the oldest stored one. Five fills
     * one writer batch, so the store flushes without the 60 s timeout. */
    power_history_record_t older[5];
    for (int i = 0; i < 5; ++i) {
        older[i] = make_record((uint32_t)(i + 1), oldest - (int64_t)(5 - i) * 60,
                               3000 + i);
    }
    size_t merged = 0;
    CHECK(power_history_merge(older, 5, &merged) == ESP_OK, "older merge returns OK");
    wait_for_quiet();

    power_history_get_status(&st);
    CHECK(st.record_count == total, "a full ring did not grow past capacity");

    /* The newest minute MUST survive; the older backfill minute must not be
     * stored (there is no room for it without evicting a newer minute). */
    CHECK(power_history_has_minute(newest), "newest minute survived the backfill");
    CHECK(power_history_has_minute(oldest), "oldest minute still present");
    CHECK(!power_history_has_minute(oldest - 300), "older backfill minute was dropped");

    /* Walk the whole ring: strictly ascending, no duplicate, newest last. */
    power_history_record_t page[POWER_HISTORY_PAGE_SIZE];
    size_t count = 0;
    bool more = false;
    int64_t cursor = 0;
    uint32_t seen = 0;
    bool ascending = true;
    bool dup = false;
    int64_t last = 0;
    bool have_last = false;
    for (;;) {
        CHECK(power_history_read_after(cursor, POWER_HISTORY_PAGE_SIZE, page,
                                       POWER_HISTORY_PAGE_SIZE, &count, &st,
                                       &more) == ESP_OK,
              "read_after ok");
        for (size_t i = 0; i < count; ++i) {
            if (have_last && page[i].interval_start_utc <= last) {
                ascending = false;
                if (page[i].interval_start_utc == last) {
                    dup = true;
                }
            }
            last = page[i].interval_start_utc;
            have_last = true;
            ++seen;
        }
        if (count != 0) {
            cursor = page[count - 1].interval_start_utc;
        }
        if (!more) {
            break;
        }
    }
    CHECK(seen == total, "the full ring still holds 29000 records");
    CHECK(ascending && !dup, "ring is time-sorted and duplicate-free after backfill");
    CHECK(last == newest, "the newest minute is the tail of the ring");

    printf("history_full_ring_backfill: %d checks, %d failures\n", g_checks,
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
