/* Host test for the persisted "last known wall clock".
 *
 * Symptom it guards against: after a reboot without a fresh SNTP/ESP-NOW sync
 * the device has no wall clock, so the minute recorder's bucket_start is 0 and
 * every sample of that boot is silently dropped. The fix persists the newest
 * wall clock in NVS and seeds the clock from it on the next boot.
 *
 * Two halves:
 *   1. the pure record/clock math (wall_clock.c), and
 *   2. the real NVS store (wall_clock_store.c) driven through a file-backed NVS
 *      shim, with the "reboot" done by forking a fresh process - the store lives
 *      in process-local statics, exactly like a real reset.
 *
 * Acceptance: write with a known time, reboot, the clock still advances and
 * bucket_start is never 0.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nvs.h"
#include "wall_clock.h"

/* --- file-backed NVS shim (implements test/host/include/nvs.h) ------------- */

#define NVS_MAX_OPEN 8
static char g_nvs_names[NVS_MAX_OPEN][32];
static bool g_nvs_used[NVS_MAX_OPEN];

static void nvs_path(char *out, size_t size, const char *name, const char *key)
{
    snprintf(out, size, "%s/%s.%s", WALL_CLOCK_TEST_DIR, name, key);
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    for (int i = 0; i < NVS_MAX_OPEN; ++i) {
        if (!g_nvs_used[i]) {
            g_nvs_used[i] = true;
            snprintf(g_nvs_names[i], sizeof(g_nvs_names[i]), "%s", name);
            *out = i + 1;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    if (handle < 1 || handle > NVS_MAX_OPEN) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[128];
    nvs_path(path, sizeof(path), g_nvs_names[handle - 1], key);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }
    size_t written = fwrite(value, 1, length, f);
    fclose(f);
    return written == length ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *length)
{
    if (handle < 1 || handle > NVS_MAX_OPEN || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[128];
    nvs_path(path, sizeof(path), g_nvs_names[handle - 1], key);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t want = *length;
    size_t got = fread(out, 1, want, f);
    fclose(f);
    *length = got;
    return got == want ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    if (handle >= 1 && handle <= NVS_MAX_OPEN) {
        g_nvs_used[handle - 1] = false;
    }
}

/* --- test harness --------------------------------------------------------- */

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

#define KNOWN_UTC 1750000000LL /* 2025-06-15T14:26:40Z, a plausible date */

static void test_record_validation(void)
{
    printf("test_record_validation\n");
    CHECK(!wall_clock_utc_is_sane(0), "utc 0 is rejected");
    CHECK(!wall_clock_utc_is_sane(WALL_CLOCK_MIN_UTC - 1), "just below the floor is rejected");
    CHECK(wall_clock_utc_is_sane(WALL_CLOCK_MIN_UTC), "the floor itself is accepted");
    CHECK(wall_clock_utc_is_sane(KNOWN_UTC), "a real date is accepted");
    CHECK(!wall_clock_utc_is_sane(WALL_CLOCK_MAX_UTC + 1), "past the ceiling is rejected");

    wall_clock_record_t record;
    wall_clock_make_record(&record, KNOWN_UTC);
    CHECK(wall_clock_record_is_valid(&record), "a freshly made record is valid");
    CHECK(record.magic == WALL_CLOCK_MAGIC, "record carries the magic");
    CHECK(record.version == WALL_CLOCK_VERSION, "record carries the current version");

    wall_clock_record_t corrupt = record;
    corrupt.utc_seconds += 1; /* checksum no longer matches */
    CHECK(!wall_clock_record_is_valid(&corrupt), "a tampered time fails the checksum");

    wall_clock_record_t wrong_version = record;
    wrong_version.version = WALL_CLOCK_VERSION + 1;
    CHECK(!wall_clock_record_is_valid(&wrong_version), "a foreign version is rejected");

    wall_clock_record_t bad_time = record;
    bad_time.utc_seconds = 0;
    wall_clock_make_record(&bad_time, 0); /* valid checksum over an insane time */
    CHECK(!wall_clock_record_is_valid(&bad_time), "a checksummed but insane time is rejected");

    CHECK(!wall_clock_record_is_valid(NULL), "a NULL record is rejected");
}

static void test_clock_advances(void)
{
    printf("test_clock_advances\n");
    /* Seed at boot (uptime 0) from the persisted time. */
    int64_t offset_us = wall_clock_offset_us_from_utc(KNOWN_UTC, 0);
    CHECK(wall_clock_utc_ms_from_offset(offset_us, 0) == KNOWN_UTC * 1000,
          "seeded clock equals the persisted time at boot");

    /* The clock must keep advancing on its own after the reboot. */
    CHECK(wall_clock_utc_ms_from_offset(offset_us, 60000) == (KNOWN_UTC + 60) * 1000,
          "seeded clock advances with uptime (one minute later)");
    CHECK(wall_clock_utc_ms_from_offset(offset_us, 3600000) == (KNOWN_UTC + 3600) * 1000,
          "seeded clock advances with uptime (one hour later)");

    /* A calibration at a non-zero uptime produces the same clock. */
    int64_t later_offset = wall_clock_offset_us_from_utc(KNOWN_UTC + 120, 120000);
    CHECK(later_offset == offset_us, "offset is invariant to the uptime it is derived at");
}

static void test_bucket_start(void)
{
    printf("test_bucket_start\n");
    CHECK(wall_clock_bucket_start_utc(KNOWN_UTC, 60) == KNOWN_UTC - (KNOWN_UTC % 60),
          "bucket start floors to the minute");
    CHECK(wall_clock_bucket_start_utc(KNOWN_UTC, 60) % 60 == 0,
          "bucket start is a whole minute");
    CHECK(wall_clock_bucket_start_utc(KNOWN_UTC, 60) != 0,
          "a seeded clock yields a non-zero bucket_start (the whole point)");
    CHECK(wall_clock_bucket_start_utc(KNOWN_UTC, 0) == KNOWN_UTC,
          "a non-positive interval does not divide by zero");

    /* The bug: an invalid clock is 0 and its bucket is 0, so nothing is stored. */
    CHECK(wall_clock_bucket_start_utc(0, 60) == 0, "an invalid clock still floors to 0");
}

static void test_persist_due(void)
{
    printf("test_persist_due\n");
    CHECK(wall_clock_persist_due(0, KNOWN_UTC, 300), "nothing saved yet -> save");
    CHECK(!wall_clock_persist_due(KNOWN_UTC, KNOWN_UTC, 300), "same second -> skip");
    CHECK(!wall_clock_persist_due(KNOWN_UTC, KNOWN_UTC + 299, 300), "under the interval -> skip");
    CHECK(wall_clock_persist_due(KNOWN_UTC, KNOWN_UTC + 300, 300), "at the interval -> save");
    CHECK(wall_clock_persist_due(KNOWN_UTC, KNOWN_UTC - 10, 300),
          "a backwards correction is saved immediately");
}

/* Child: "reboot" - a fresh process loads the store and reports whether it can
 * seed a clock that advances. Exits 0 and prints the restored utc + a later
 * clock value, or a non-zero code on failure. */
static int reboot_phase(void)
{
    int64_t restored = 0;
    bool found = false;
    if (wall_clock_store_load(&restored, &found) != ESP_OK) {
        fprintf(stderr, "reboot_phase: load failed\n");
        return 21;
    }
    if (!found) {
        fprintf(stderr, "reboot_phase: no stored clock after reboot\n");
        return 22;
    }
    if (restored != KNOWN_UTC) {
        fprintf(stderr, "reboot_phase: restored %lld, expected %lld\n",
                (long long)restored, (long long)KNOWN_UTC);
        return 23;
    }
    /* Seed the clock from the persisted value and let it run for 90 s. */
    int64_t offset_us = wall_clock_offset_us_from_utc(restored, 0);
    int64_t now_ms = wall_clock_utc_ms_from_offset(offset_us, 90000);
    if (wall_clock_bucket_start_utc(now_ms / 1000, 60) == 0) {
        fprintf(stderr, "reboot_phase: bucket_start is 0 after reboot\n");
        return 24;
    }
    printf("%lld\n", (long long)now_ms);
    return 0;
}

static int run_reboot_child(int64_t *now_ms_out)
{
    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int rc = reboot_phase();
        fflush(stdout);
        _exit(rc);
    }
    close(fds[1]);
    char buf[64] = {0};
    ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    if (now_ms_out != NULL) {
        *now_ms_out = atoll(buf);
    }
    return 0;
}

static void test_store_round_trip(void)
{
    printf("test_store_round_trip\n");

    /* Nothing stored yet. */
    int64_t value = -1;
    bool found = true;
    CHECK(wall_clock_store_load(&value, &found) == ESP_OK && !found,
          "an empty store reports not found");

    /* Write with a known time. */
    CHECK(wall_clock_store_save(KNOWN_UTC) == ESP_OK, "saving a known time succeeds");
    CHECK(wall_clock_store_load(&value, &found) == ESP_OK && found && value == KNOWN_UTC,
          "the known time reads back");

    /* An insane time is refused and does not overwrite the good one. */
    CHECK(wall_clock_store_save(0) == ESP_ERR_INVALID_ARG, "an insane time is refused");
    CHECK(wall_clock_store_save(WALL_CLOCK_MAX_UTC + 1) == ESP_ERR_INVALID_ARG,
          "a time past the ceiling is refused");
    CHECK(wall_clock_store_load(&value, &found) == ESP_OK && found && value == KNOWN_UTC,
          "a refused save leaves the stored time intact");

    /* Reboot: a fresh process must seed from the stored clock. */
    int64_t now_ms = 0;
    CHECK(run_reboot_child(&now_ms) == 0, "the reboot child seeds from the stored clock");
    CHECK(now_ms == (KNOWN_UTC + 90) * 1000,
          "after the reboot the clock has advanced by the elapsed uptime");
    CHECK(now_ms / 1000 != 0 && wall_clock_bucket_start_utc(now_ms / 1000, 60) != 0,
          "after the reboot bucket_start is never 0");
}

static void test_store_rejects_corruption(void)
{
    printf("test_store_rejects_corruption\n");
    /* Overwrite the stored blob with garbage of the right size: load must
     * report not-found instead of seeding from a corrupt time. */
    CHECK(wall_clock_store_save(KNOWN_UTC) == ESP_OK, "re-seed a good time");
    nvs_handle_t handle;
    CHECK(nvs_open("wallclock", NVS_READWRITE, &handle) == ESP_OK, "open the namespace");
    uint8_t garbage[sizeof(wall_clock_record_t)];
    memset(garbage, 0xA5, sizeof(garbage));
    CHECK(nvs_set_blob(handle, "last", garbage, sizeof(garbage)) == ESP_OK,
          "write a corrupt blob");
    nvs_commit(handle);
    nvs_close(handle);

    int64_t value = 123;
    bool found = true;
    CHECK(wall_clock_store_load(&value, &found) == ESP_OK && !found,
          "a corrupt stored clock is ignored, not seeded");
    CHECK(value == 0, "a corrupt store leaves the out value at 0");
}

static void cleanup(void)
{
    char path[128];
    nvs_path(path, sizeof(path), "wallclock", "last");
    remove(path);
    rmdir(WALL_CLOCK_TEST_DIR);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mkdir(WALL_CLOCK_TEST_DIR, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "could not create %s: %s\n", WALL_CLOCK_TEST_DIR, strerror(errno));
        return 2;
    }
    atexit(cleanup);

    test_record_validation();
    test_clock_advances();
    test_bucket_start();
    test_persist_due();
    test_store_round_trip();
    test_store_rejects_corruption();

    printf("  wall-clock: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
