/* Automatic storage-recovery host test for the power-history store.
 *
 * The receiver's SPIFFS partition can end up unusable (a hard reset mid-write
 * leaves the index inconsistent), so power_history_init() falls back to a
 * RAM-only store and every reboot drops the history. The background repair task
 * must bring such a store back to a persistent one, formatting the partition as
 * the last resort (the sender backfill refills the minutes).
 *
 * The host shim mounts nothing real: esp_vfs_spiffs_register() reports success
 * and the driver opens POWER_HISTORY_FILE as a plain file. So the way to make
 * the file unopenable here is to put a *directory* at the file path: both
 * fopen("r+b") and fopen("w+b") fail, exactly like a damaged SPIFFS object.
 * power_history_repair_ring() then retries the recovery; once the obstacle is
 * gone it recreates the file with fresh headers and the store is persistent
 * again.
 *
 * Runs against a scratch mount point under the build tmp dir (the runner passes
 * -DPOWER_HISTORY_MOUNT_PATH); nothing outside the repo is touched.
 */
#include <errno.h>
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

static power_history_record_t make_record(uint32_t seq, int32_t mw)
{
    power_history_record_t r;
    memset(&r, 0, sizeof(r));
    r.sequence = seq;
    r.interval_start_utc = 6000000000LL + (int64_t)seq * 60;
    r.average_milliwatts = mw;
    r.sample_count = 60;
    r.format_version = POWER_HISTORY_VERSION;
    /* checksum left 0: the store recomputes it on write (like the live path). */
    return r;
}

/* Wait until the writer task has drained `want` records into the store. */
static bool wait_record_count(uint32_t want)
{
    power_history_status_t st;
    for (int i = 0; i < 300; ++i) {
        power_history_get_status(&st);
        if (st.record_count == want) {
            return true;
        }
        usleep(10000);
    }
    return false;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (mkdir(POWER_HISTORY_MOUNT_PATH, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "could not create %s: %s\n",
                POWER_HISTORY_MOUNT_PATH, strerror(errno));
        return 2;
    }
    /* Make the history file path unopenable: a directory cannot be fopen()ed,
     * so both "r+b" and "w+b" fail, as they do on a damaged SPIFFS object. */
    (void)rmdir(POWER_HISTORY_FILE);
    if (mkdir(POWER_HISTORY_FILE, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "could not create %s: %s\n",
                POWER_HISTORY_FILE, strerror(errno));
        return 2;
    }

    printf("test_storage_recovers_after_format\n");

    /* Boot with an unusable file: the store must come up, but RAM-only. */
    CHECK(power_history_init() == ESP_OK, "init succeeds even with an unopenable file");
    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.ready, "store is ready (writer task running)");
    CHECK(!st.persistent, "store is RAM-only while the file cannot be opened");

    /* The automatic recovery cannot help yet (the obstacle is still there), and
     * it must not wedge or crash. */
    CHECK(power_history_repair_ring() == ESP_OK, "repair returns OK, does not wedge");
    power_history_get_status(&st);
    CHECK(!st.persistent, "still RAM-only while the file path is a directory");

    /* Remove the obstacle; the next repair must recreate the file with fresh
     * headers and make the store persistent again. */
    CHECK(rmdir(POWER_HISTORY_FILE) == 0, "obstacle removed");
    CHECK(power_history_repair_ring() == ESP_OK, "repair after obstacle returns OK");
    power_history_get_status(&st);
    CHECK(st.persistent, "store is persistent again after the automatic format");
    CHECK(st.record_count == 0, "recovered store starts empty (backfill refills it)");

    /* And it must actually persist new minutes now. Use a full batch
     * (POWER_HISTORY_BATCH_RECORDS): the writer only flushes when the batch is
     * full or its 60 s timeout fires, so a partial batch would just wait. */
    for (int i = 1; i <= 5; ++i) {
        power_history_record_t r = make_record((uint32_t)i, 1000 + i);
        CHECK(power_history_enqueue(&r), "enqueue into the recovered store");
    }
    CHECK(wait_record_count(5), "recovered store persists the new minutes");
    power_history_get_status(&st);
    CHECK(st.persistent, "still persistent after writing");

    /* A repair on a healthy store is a no-op (idempotent). */
    CHECK(power_history_repair_ring() == ESP_OK, "repair on a healthy store returns OK");
    power_history_get_status(&st);
    CHECK(st.persistent && st.record_count == 5, "healthy store unchanged by the repair");

    /* The explicit console 'test storage format' primitive must also work and
     * leave a persistent, empty store. */
    CHECK(power_history_format() == ESP_OK, "explicit format succeeds");
    power_history_get_status(&st);
    CHECK(st.persistent, "explicit format leaves a persistent store");
    CHECK(st.record_count == 0, "explicit format empties the store");

    remove(POWER_HISTORY_FILE);
    rmdir(POWER_HISTORY_MOUNT_PATH);

    printf("  storage-recovery: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
