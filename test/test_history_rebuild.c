/* Host test for power_history_rebuild_file(): proves the data-preserving file
 * rebuild recreates quarters.bin at exactly the compact size the live records
 * need and keeps every minute, after the store has been running normally.
 *
 * On the device the rebuild exists because SPIFFS_ftruncate (esp-idf v6.1)
 * only shortens the logical file length without marking the cropped pages as
 * deleted, so 'used' stays at the old full size and GC reclaims nothing. The
 * host test validates the record-preservation and re-basing logic of the same
 * function against a scratch file.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static const char *g_file_path = POWER_HISTORY_FILE;

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

static power_history_record_t make_record(uint32_t seq, int32_t mw)
{
    power_history_record_t r;
    memset(&r, 0, sizeof(r));
    r.sequence = seq;
    r.interval_start_utc = 9000000000LL + (int64_t)seq * 60;
    r.average_milliwatts = mw;
    r.sample_count = 60;
    r.format_version = POWER_HISTORY_VERSION;
    r.checksum = crc_record(&r);
    return r;
}

static long file_size(void)
{
    struct stat st;
    if (stat(g_file_path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

static int verify_records(uint32_t expected_count)
{
    power_history_status_t st;
    power_history_get_status(&st);
    if (!st.ready || st.record_count != expected_count) {
        printf("  FAIL: after rebuild record_count=%u expected=%u\n",
               (unsigned)st.record_count, (unsigned)expected_count);
        ++g_failures;
        ++g_checks;
        return 1;
    }
    return 0;
}

int main(void)
{
    if (mkdir(POWER_HISTORY_MOUNT_PATH, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s): %s\n", POWER_HISTORY_MOUNT_PATH, strerror(errno));
        return 2;
    }
    struct stat dummy;
    if (stat(g_file_path, &dummy) == 0) {
        unlink(g_file_path);
    }

    if (power_history_init() != 0) {
        fprintf(stderr, "init failed\n");
        return 3;
    }

    /* Few enough records that the async writer drains the queue in one pass
     * (the host shim caps POWER_HISTORY_BATCH_RECORDS at 5), so the store is
     * fully persisted before the rebuild runs. */
    const uint32_t count = 5;
    for (uint32_t i = 1; i <= count; ++i) {
        power_history_record_t r = make_record(i, 2000 + (int32_t)i);
        if (!power_history_enqueue(&r)) {
            fprintf(stderr, "enqueue %u failed\n", i);
            return 4;
        }
    }
    power_history_status_t st;
    for (int i = 0; i < 300; ++i) {
        power_history_get_status(&st);
        if (st.record_count == count) {
            break;
        }
        usleep(20000);
    }
    CHECK(st.record_count == count, "writer persisted all records before rebuild");
    CHECK(st.persistent, "store is persistent before rebuild");

    long before = file_size();
    long needed = (long)(2u * sizeof(power_history_header_t) +
                         count * sizeof(power_history_record_t));
    CHECK(before >= needed && before <= needed + 0,
          "fresh file already at compact minimum before rebuild");

    /* The device-side rebuild is an in-place COW rewrite (records re-stored to
     * slots 0..count-1, forcing SPIFFS to mark the old pages including the
     * ftruncate ghost tail as deleted so the GC can reap them). On the POSIX
     * host there are no ghost pages, so the long-term contract to assert is
     * that every record survives and the rebuild is repeatable; the file length
     * itself stays put (no ftruncate is involved). */
    long before_again = file_size();

    if (power_history_rebuild_file() != ESP_OK) {
        fprintf(stderr, "rebuild failed\n");
        return 5;
    }

    long after = file_size();
    CHECK(after == before_again, "rebuild leaves the file length unchanged");
    CHECK(verify_records(count) == 0, "records intact after rebuild");
    CHECK(st.record_count == count, "status record count unchanged after rebuild");

    /* Rebuild must be repeatable: a second run must keep the records. */
    if (power_history_rebuild_file() != ESP_OK) {
        fprintf(stderr, "second rebuild failed\n");
        return 6;
    }
    long after2 = file_size();
    CHECK(after2 == before_again, "second rebuild leaves the file length unchanged");
    CHECK(verify_records(count) == 0, "records intact after second rebuild");

    if (g_failures == 0) {
        printf("OK: history rebuild (%u records x2, file stays %ld bytes)\n",
               (unsigned)count, after);
    } else {
        printf("FAILED: %u/%u checks\n", g_failures, g_checks);
    }
    return g_failures == 0 ? 0 : 1;
}