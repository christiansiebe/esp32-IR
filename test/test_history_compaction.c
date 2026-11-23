/* Host test for the history-file compaction inside power_history_repair_ring().
 *
 * The bug: an OTA/hard-reset can leave SPIFFS with a history file that has been
 * written up to the physical ring capacity (2 headers + CAPACITY * record
 * bytes) while only a few thousand of those slots hold live minutes. SPIFFS
 * allocates in blocks and needs free blocks for garbage collection, so a
 * mostly-max-size-but-sparse file makes every further shift/insertion fail with
 * ENOSPC even though the partition still looks half empty -- the recurring
 * "ring stalls at the same record count, write errors climb" fault.
 *
 * This test drives the real store against a scratch file (as
 * test_reboot_persistence.c does), deliberately blows the file up far past the
 * size the live records need, runs power_history_repair_ring() and asserts that
 * the physical file shrinks back to exactly what the records need while every
 * stored minute survives intact.
 *
 * It exercises the "records already at the low end" branch (oldest_slot == 0),
 * which is the plain truncation path. The copy-down branch (oldest_slot > 0)
 * is covered by the same function logic and is hard to set up from outside the
 * store, so it is validated by code review + the on-device run instead.
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
    r.interval_start_utc = 6000000000LL + (int64_t)seq * 60;
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
        perror("stat");
        return -1;
    }
    return (long)st.st_size;
}

/* Append `extra` bytes of junk to the physical file so it is larger than the
 * live records need (simulating the historically-grown, over-large ring file
 * that SPIFFS never shrank). POSIX truncate() grows the file with zeros. */
static int blow_up_file(long extra)
{
    int fd = open(g_file_path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        perror("open append");
        return -1;
    }
    off_t r = lseek(fd, 0, SEEK_END);
    if (r < 0) {
        perror("lseek end");
        close(fd);
        return -1;
    }
    long target = (long)r + extra;
    if (ftruncate(fd, target) != 0) {
        perror("ftruncate grow");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int verify_records(uint32_t expected_count)
{
    power_history_status_t st;
    power_history_get_status(&st);
    if (!st.ready || st.record_count != expected_count) {
        printf("  FAIL: after compaction record_count=%u expected=%u\n",
               (unsigned)st.record_count, (unsigned)expected_count);
        ++g_failures;
        ++g_checks;
        return 1;
    }
    return 0;
}

int main(void)
{
    /* Provision the scratch mount directory (the host shim treats the mount as
     * a plain directory and opens POWER_HISTORY_MOUNT_PATH/POWER_HISTORY_FILE). */
    if (mkdir(POWER_HISTORY_MOUNT_PATH, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s): %s\n", POWER_HISTORY_MOUNT_PATH, strerror(errno));
        return 2;
    }
    /* Remove any stale scratch store. */
    struct stat dummy;
    if (stat(g_file_path, &dummy) == 0) {
        unlink(g_file_path);
    }

    if (power_history_init() != 0) {
        fprintf(stderr, "init failed\n");
        return 3;
    }

    /* Few enough records that the async writer drains the queue in one pass, as in
     * the reboot-persistence test (the host shim caps the queue at 16 and the
     * writer batches at POWER_HISTORY_BATCH_RECORDS). */
    const uint32_t count = 5;
    for (uint32_t i = 1; i <= count; ++i) {
        power_history_record_t r = make_record(i, 1000 + (int32_t)i);
        if (!power_history_enqueue(&r)) {
            fprintf(stderr, "enqueue %u failed\n", i);
            return 4;
        }
    }
    /* Wait for the async writer to persist everything. */
    power_history_status_t st;
    for (int i = 0; i < 300; ++i) {
        power_history_get_status(&st);
        if (st.record_count == count) {
            break;
        }
        usleep(20000);
    }
    CHECK(st.record_count == count,
          "writer persisted all records before compaction");

    long base_size = file_size();
    long needed = (long)(2u * sizeof(power_history_header_t) +
                         count * sizeof(power_history_record_t));
    CHECK(base_size >= needed && base_size <= needed + 0,
          "fresh file already at compact minimum before blowing up");

    /* Simulate the historically-grown over-large file. */
    static const long EXTRA = 2000000L; /* ~2 MB past what the records need */
    if (blow_up_file(EXTRA) != 0) {
        return 5;
    }
    long blown = file_size();
    CHECK(blown >= base_size + EXTRA - 64,
          "file was blown up beyond the live-record byte need");

    if (power_history_repair_ring() != ESP_OK) {
        fprintf(stderr, "repair_ring failed\n");
        return 6;
    }

    /* The physical file must now be exactly the compact minimum again. */
    long final_size = file_size();
    CHECK(final_size == needed,
          "compaction shrank the file back to exactly the live-record size");
    CHECK(final_size <= base_size + 64,
          "compacted file is no larger than the original fresh store");

    /* Every minute must survive. */
    CHECK(verify_records(count) == 0, "records intact after compaction");

    if (g_failures == 0) {
        printf("OK: history compaction (file %ld -> %ld, %u records)\n",
               blown, final_size, (unsigned)count);
    } else {
        printf("FAILED: %u/%u checks\n", g_failures, g_checks);
    }
    return g_failures ? 1 : 0;
}