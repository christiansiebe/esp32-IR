/* Host test for the day/month running energy totals.
 *
 * Symptom it guards against: the minute ring only holds ~20 days, so a
 * per-day/per-month view would have to re-scan the whole ring every time and
 * still lose anything older. The fix keeps a compact running total per UTC day
 * and month, folded from every stored minute (including backfilled minutes).
 *
 * Two halves:
 *   1. the pure accumulator (energy_totals.c): day/month split, rollover,
 *      exclusion of boot markers, ring eviction and the calendar math, and
 *   2. the real NVS store (energy_totals_store.c) driven through a file-backed
 *      NVS shim, with the "reboot" done by forking a fresh process - the store
 *      lives in process-local statics, exactly like a real reset.
 *
 * Acceptance: a measured minute adds to its day and month; a boot marker does
 * not; a reboot restores the totals; and the 60 s * 1000 mW = 1 Wh identity
 * holds exactly.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "energy_totals.h"
#include "nvs.h"

/* --- file-backed NVS shim (implements test/host/include/nvs.h) ------------- */

#define NVS_MAX_OPEN 8
static char g_nvs_names[NVS_MAX_OPEN][32];
static bool g_nvs_used[NVS_MAX_OPEN];

static void nvs_path(char *out, size_t size, const char *name, const char *key)
{
    snprintf(out, size, "%s/%s.%s", ENERGY_TOTALS_TEST_DIR, name, key);
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

#define CHECK_NEAR(a, b, eps, msg)                                      \
    do {                                                                \
        ++g_checks;                                                     \
        double _a = (a), _b = (b);                                      \
        if (!(_a - _b <= (eps) && _b - _a <= (eps))) {                  \
            ++g_failures;                                               \
            printf("  FAIL: %s (got %.6f want %.6f) (%s:%d)\n",         \
                   msg, _a, _b, __FILE__, __LINE__);                    \
        }                                                               \
    } while (0)

/* 2025-06-15T14:26:40Z, a plausible date (matches the wall-clock test). */
#define KNOWN_UTC 1750000000LL

static power_history_record_t measured_record(int64_t minute_utc, int32_t mw)
{
    power_history_record_t r = {0};
    r.interval_start_utc = minute_utc;
    r.average_milliwatts = mw;
    r.sample_count = 60;
    r.reason = POWER_HISTORY_REASON_NONE;
    r.format_version = POWER_HISTORY_VERSION;
    return r;
}

/* --- pure accumulator tests ---------------------------------------------- */

static void test_calendar_math(void)
{
    printf("test_calendar_math\n");
    /* A whole minute exactly on midnight floors to itself. */
    CHECK(energy_totals_day_start_utc(1750000000LL) == 1749945600LL,
          "day start is midnight UTC");
    CHECK(energy_totals_day_start_utc(1749945600LL) == 1749945600LL,
          "midnight floors to itself");
    CHECK(energy_totals_day_start_utc(1749945599LL) == 1749859200LL,
          "one second before midnight is the previous day");

    /* 2025-06-15 -> index 2025*12 + 5. */
    CHECK(energy_totals_month_index(1750000000LL) == 2025 * 12 + 5,
          "June 2025 has the expected month index");
    /* Month rollover: 2025-01-01 vs 2024-12-31 must differ by one index. */
    int32_t jan = energy_totals_month_index(1735689600LL);  /* 2025-01-01 */
    int32_t dec = energy_totals_month_index(1735603200LL);  /* 2024-12-31 */
    CHECK(jan == dec + 1, "month index increments across the year boundary");
    CHECK(jan == 2025 * 12, "January is month 0 of its year");
}

static void test_accumulation(void)
{
    printf("test_accumulation\n");
    energy_totals_t totals;
    energy_totals_reset(&totals);

    /* 60 minutes at 1000 mW = 1 Wh exactly. */
    for (int i = 0; i < 60; ++i) {
        power_history_record_t r = measured_record(1750000000LL + (int64_t)i * 60,
                                                   1000);
        CHECK(energy_totals_add(&totals, &r), "a measured minute counts");
    }
    CHECK_NEAR(energy_totals_day_wh(&totals, 1750000000LL), 1.0, 1e-9,
               "60 minutes at 1 W is exactly 1 Wh");
    CHECK_NEAR(energy_totals_month_wh(&totals, 1750000000LL), 1.0, 1e-9,
               "the same minute also lands in the month total");

    /* A second, lower-power day must not change the first day's total. */
    int64_t next_day = 1750000000LL + 86400;
    for (int i = 0; i < 30; ++i) {
        power_history_record_t r = measured_record(next_day + (int64_t)i * 60, 500);
        energy_totals_add(&totals, &r);
    }
    CHECK_NEAR(energy_totals_day_wh(&totals, 1750000000LL), 1.0, 1e-9,
               "yesterday is untouched by today");
    CHECK_NEAR(energy_totals_day_wh(&totals, next_day), 0.25, 1e-9,
               "30 minutes at 0.5 W is 0.25 Wh");
    /* June 15 + 1 day is still June (unless it crossed the month). */
    if (energy_totals_month_index(next_day) == energy_totals_month_index(1750000000LL)) {
        CHECK_NEAR(energy_totals_month_wh(&totals, next_day), 1.25, 1e-9,
                   "the month total sums both days");
    }

    CHECK(totals.folded_through_utc == next_day + 29 * 60,
          "folded_through tracks the newest counted minute");
}

static void test_boot_markers_excluded(void)
{
    printf("test_boot_markers_excluded\n");
    energy_totals_t totals;
    energy_totals_reset(&totals);

    power_history_record_t measured = measured_record(1750000000LL, 1000);
    CHECK(energy_totals_add(&totals, &measured), "a measured minute counts");

    /* A boot marker (reason != NONE) stands for an offline minute, not for zero
     * consumption: it must add nothing. */
    power_history_record_t marker = {0};
    marker.interval_start_utc = 1750000000LL + 60;
    marker.average_milliwatts = 0;
    marker.sample_count = 0;
    marker.reason = POWER_HISTORY_REASON_OTA;
    CHECK(!energy_totals_add(&totals, &marker), "an OTA boot marker is ignored");

    /* A record with no samples is a marker even without a reason set. */
    power_history_record_t no_samples = measured_record(1750000000LL + 120, 1000);
    no_samples.sample_count = 0;
    CHECK(!energy_totals_add(&totals, &no_samples), "a sample-less record is ignored");

    /* A pre-2024 minute (no wall clock) is not a real measurement. */
    power_history_record_t epoch = measured_record(0, 1000);
    CHECK(!energy_totals_add(&totals, &epoch), "a 1970 minute is rejected");

    CHECK(totals.days[0].minute_count == 1, "only the one measured minute counted");
    CHECK_NEAR(energy_totals_day_wh(&totals, 1750000000LL), 1.0 / 60.0, 1e-9,
               "1 minute at 1 W is 1/60 Wh");
}

static void test_blob_roundtrip(void)
{
    printf("test_blob_roundtrip\n");
    energy_totals_t totals;
    energy_totals_reset(&totals);
    for (int i = 0; i < 10; ++i) {
        power_history_record_t r = measured_record(1750000000LL + (int64_t)i * 60, 2000);
        energy_totals_add(&totals, &r);
    }

    energy_totals_blob_t blob;
    energy_totals_make_blob(&totals, &blob);
    CHECK(energy_totals_blob_is_valid(&blob), "a freshly made blob is valid");
    CHECK(blob.magic == ENERGY_TOTALS_MAGIC, "blob carries the magic");
    CHECK(blob.version == ENERGY_TOTALS_VERSION, "blob carries the version");

    energy_totals_t restored;
    CHECK(energy_totals_from_blob(&blob, &restored), "a valid blob restores");
    CHECK_NEAR(energy_totals_day_wh(&restored, 1750000000LL),
               energy_totals_day_wh(&totals, 1750000000LL), 1e-9,
               "restored day total matches");
    CHECK(restored.folded_through_utc == totals.folded_through_utc,
          "restored fold cursor matches");

    energy_totals_blob_t corrupt = blob;
    corrupt.folded_through_utc += 1; /* checksum no longer matches */
    CHECK(!energy_totals_blob_is_valid(&corrupt), "a tampered blob fails the checksum");
    energy_totals_t cleared;
    CHECK(!energy_totals_from_blob(&corrupt, &cleared), "an invalid blob is rejected");

    energy_totals_blob_t wrong_version = blob;
    wrong_version.version = ENERGY_TOTALS_VERSION + 1;
    CHECK(!energy_totals_blob_is_valid(&wrong_version), "a foreign version is rejected");
}

static void test_ring_eviction(void)
{
    printf("test_ring_eviction\n");
    energy_totals_t totals;
    energy_totals_reset(&totals);

    /* Fill more days than the ring has slots; the oldest must be evicted while
     * the newest day is still exact. */
    int64_t base = 1750000000LL;
    for (size_t i = 0; i < ENERGY_TOTALS_DAY_SLOTS + 5; ++i) {
        int64_t day = base + (int64_t)i * 86400;
        power_history_record_t r = measured_record(day, 60000); /* 1 Wh per minute */
        energy_totals_add(&totals, &r);
    }
    int64_t newest = base + (int64_t)(ENERGY_TOTALS_DAY_SLOTS + 4) * 86400;
    CHECK_NEAR(energy_totals_day_wh(&totals, newest), 1.0, 1e-9,
               "the newest day is exact after eviction");
    CHECK_NEAR(energy_totals_day_wh(&totals, base), 0.0, 1e-9,
               "the oldest day was evicted");
}

/* --- real NVS store: write, "reboot" (fork), read back -------------------- */

static void test_store_reboot(void)
{
    printf("test_store_reboot\n");

    energy_totals_t totals;
    energy_totals_reset(&totals);
    for (int i = 0; i < 60; ++i) {
        power_history_record_t r = measured_record(1750000000LL + (int64_t)i * 60, 1000);
        energy_totals_add(&totals, &r);
    }
    CHECK(energy_totals_store_save(&totals) == ESP_OK, "the totals save to NVS");

    /* Fork a fresh process: the in-RAM totals are gone, exactly like a reset.
     * The child loads from the store and reports its verdict through the exit
     * code. */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0, "fork succeeds");
    if (pid == 0) {
        energy_totals_t loaded;
        bool found = false;
        esp_err_t err = energy_totals_store_load(&loaded, &found);
        double wh = energy_totals_day_wh(&loaded, 1750000000LL);
        int ok = (err == ESP_OK) && found && (wh > 0.999999 && wh < 1.000001);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "after a reboot the day total is restored from NVS");

    /* A missing store must report found=false and leave the totals cleared. */
    unlink(ENERGY_TOTALS_TEST_DIR "/energy.totals");
    energy_totals_t empty;
    bool found = true;
    CHECK(energy_totals_store_load(&empty, &found) == ESP_OK, "load without a store is ok");
    CHECK(!found, "a missing store reports found=false");
    CHECK_NEAR(energy_totals_day_wh(&empty, 1750000000LL), 0.0, 1e-9,
               "a missing store yields zero totals");

    /* A corrupt store must not poison the totals. */
    FILE *f = fopen(ENERGY_TOTALS_TEST_DIR "/energy.totals", "wb");
    CHECK(f != NULL, "can write a corrupt store for the test");
    if (f != NULL) {
        uint8_t junk[sizeof(energy_totals_blob_t)] = {0};
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
    }
    found = true;
    energy_totals_t after_corrupt;
    CHECK(energy_totals_store_load(&after_corrupt, &found) == ESP_OK,
          "load of a corrupt store is ok");
    CHECK(!found, "a corrupt store reports found=false");
    CHECK_NEAR(energy_totals_day_wh(&after_corrupt, 1750000000LL), 0.0, 1e-9,
               "a corrupt store yields zero totals");
}

int main(void)
{
    mkdir(ENERGY_TOTALS_TEST_DIR, 0777);

    test_calendar_math();
    test_accumulation();
    test_boot_markers_excluded();
    test_blob_roundtrip();
    test_ring_eviction();
    test_store_reboot();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
