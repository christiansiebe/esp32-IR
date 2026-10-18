/* Header-handling + migration host test for the power-history store.
 *
 * Two regressions live in the boot path of power_history_init() and were only
 * partly covered before:
 *
 *  1. A store written by an *older* firmware has an older header version. The
 *     validator used to demand `header->version == POWER_HISTORY_VERSION`, so a
 *     perfectly good history looked "empty" and the whole file was reinitialized
 *     -- the records were unreachable even though they migrate cleanly. This
 *     test writes an old-version header and asserts the boot keeps the records
 *     (record_count > 0, no RAM fallback).
 *  2. The `Persistent history ready: N of 29000` boot line must agree with the
 *     record_count the API reports, so the log can be trusted for the on-device
 *     check. The test captures the real log line and compares N to the status.
 *
 * power_history_init() may run only once per process, and every scenario needs
 * a different on-disk file, so each scenario runs in a forked child (a real
 * "boot") and reports its own checks. The store path is redirected under the
 * build tmp dir, so nothing outside the repo is touched.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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

/* The runner redirects POWER_HISTORY_FILE under the build tmp dir. */
static const char *g_file_path = POWER_HISTORY_FILE;

/* Capture of the "Persistent history ready: ..." boot line, set only inside a
 * scenario child so the sink is process-local. */
static char g_ready_line[192];
static void log_sink(const char *tag, const char *msg)
{
    (void)tag;
    if (strstr(msg, "Persistent history ready") != NULL) {
        snprintf(g_ready_line, sizeof(g_ready_line), "%s", msg);
    }
}

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

static power_history_record_t make_record(uint32_t seq, int64_t start_utc,
                                          int32_t mw, uint16_t format_version)
{
    power_history_record_t r;
    memset(&r, 0, sizeof(r));
    r.sequence = seq;
    r.interval_start_utc = start_utc;
    r.average_milliwatts = mw;
    r.sample_count = 60;
    r.format_version = format_version;
    r.checksum = crc_bytes(&r, offsetof(power_history_record_t, checksum));
    return r;
}

/* Write a store the way a given firmware version would have left it: one valid
 * header copy (copy 0) carrying `header_version`, copy 1 all-zero, then `count`
 * records in ring order (oldest at slot 0) in `record_version` format. */
static bool write_store(uint16_t header_version, size_t count,
                        uint16_t record_version)
{
    FILE *f = fopen(g_file_path, "w+b");
    if (f == NULL) {
        return false;
    }
    power_history_header_t header = {0};
    header.magic = 0x50484C31u; /* POWER_HISTORY_MAGIC */
    header.version = header_version;
    header.generation = 3;
    header.next_sequence = (uint32_t)count + 1u;
    header.record_count = (uint32_t)count;
    header.next_slot = (uint32_t)(count % POWER_HISTORY_CAPACITY);
    header.checksum = crc_bytes(&header, offsetof(power_history_header_t, checksum));
    bool ok = fwrite(&header, sizeof(header), 1, f) == 1;
    power_history_header_t zero = {0};
    ok = ok && fwrite(&zero, sizeof(zero), 1, f) == 1;
    const int64_t base = 9500000000LL;
    for (size_t i = 0; i < count && ok; ++i) {
        power_history_record_t r = make_record((uint32_t)(i + 1u),
                                               base + (int64_t)i * 60,
                                               (int32_t)(4000 + i), record_version);
        ok = fwrite(&r, sizeof(r), 1, f) == 1;
    }
    ok = fflush(f) == 0 && ok;
    fclose(f);
    return ok;
}

/* Read the active (highest-generation valid) header version from the file, so a
 * scenario can assert the boot upgraded an old header to the current one. */
static bool read_active_header_version(uint16_t *version_out)
{
    FILE *f = fopen(g_file_path, "r+b");
    if (f == NULL) {
        return false;
    }
    power_history_header_t headers[2] = {0};
    bool have[2] = {false, false};
    for (int i = 0; i < 2; ++i) {
        if (fseek(f, (long)(i * sizeof(headers[i])), SEEK_SET) == 0 &&
            fread(&headers[i], sizeof(headers[i]), 1, f) == 1) {
            have[i] = headers[i].magic == 0x50484C31u;
        }
    }
    fclose(f);
    if (!have[0] && !have[1]) {
        return false;
    }
    int pick = (have[0] && have[1])
                   ? ((int32_t)(headers[1].generation - headers[0].generation) > 0 ? 1 : 0)
                   : (have[1] ? 1 : 0);
    *version_out = headers[pick].version;
    return true;
}

/* Boot once against the file that is already on disk and run the assertions
 * shared by the "valid existing store must not look empty" scenarios. */
static void boot_and_check_records(const char *name, size_t expected,
                                   bool expect_upgrade)
{
    (void)name;
    g_ready_line[0] = '\0';
    host_log_sink = log_sink;

    CHECK(power_history_init() == ESP_OK, "init ok");

    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.ready, "store is ready (not the RAM fallback)");
    CHECK(st.record_count == expected,
          "record_count is restored (valid history is not treated as empty)");

    /* The records must be readable and migrated to the current format. */
    power_history_record_t page[16];
    size_t count = 0;
    bool more = false;
    CHECK(power_history_read_after(0, 16, page, 16, &count, &st, &more) == ESP_OK,
          "read_after ok");
    CHECK(count == expected, "every stored minute is readable after boot");
    bool migrated = true;
    for (size_t i = 0; i < count; ++i) {
        if (page[i].format_version != POWER_HISTORY_VERSION) {
            migrated = false;
        }
    }
    CHECK(migrated, "read records carry the current format version");

    /* The boot log line must agree with record_count. */
    CHECK(g_ready_line[0] != '\0', "the 'Persistent history ready' line was logged");
    unsigned long logged = 0;
    unsigned int capacity = 0;
    if (sscanf(g_ready_line, "Persistent history ready: %lu of %u minute records",
               &logged, &capacity) == 2) {
        CHECK(logged == (unsigned long)st.record_count,
              "log line N matches record_count");
        CHECK(capacity == POWER_HISTORY_CAPACITY,
              "log line names the store capacity");
    } else {
        CHECK(false, "the boot line has the expected shape");
    }

    if (expect_upgrade) {
        uint16_t version = 0;
        CHECK(read_active_header_version(&version), "header readable after boot");
        CHECK(version == POWER_HISTORY_VERSION,
              "old header version was upgraded to the current one");
    }
}

/* Scenario 1: current-version header + records. A valid store must not be seen
 * as empty and must survive the boot unchanged. */
static void scenario_current(void)
{
    CHECK(write_store(POWER_HISTORY_VERSION, 6, POWER_HISTORY_VERSION),
          "current-version store written");
    boot_and_check_records("current", 6, false);
}

/* Scenario 2/3: header and records in an older supported format (v1/v2). The
 * boot must keep the history and upgrade the header, not reinitialize. */
static void scenario_old_v2(void)
{
    CHECK(write_store(2, 5, 2), "v2 store written");
    boot_and_check_records("v2", 5, true);
}

static void scenario_old_v1(void)
{
    CHECK(write_store(1, 4, 1), "v1 store written");
    boot_and_check_records("v1", 4, true);
}

/* Scenario 4: the exact bug output -- an all-zero header. The boot must recover
 * (fresh header, ready, no RAM fallback) rather than crash or hang; the count is
 * 0 because the metadata was destroyed, which is the documented recovery. */
static void scenario_zero_header(void)
{
    FILE *f = fopen(g_file_path, "w+b");
    CHECK(f != NULL, "zero-header store opened");
    if (f != NULL) {
        uint8_t zero[2 * sizeof(power_history_header_t)] = {0};
        CHECK(fwrite(zero, sizeof(zero), 1, f) == 1, "zero header written");
        fclose(f);
    }
    g_ready_line[0] = '\0';
    host_log_sink = log_sink;
    CHECK(power_history_init() == ESP_OK, "init ok after an all-zero header");
    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.ready, "recovered store is ready (not the RAM fallback)");
    CHECK(st.record_count == 0, "a destroyed header yields an empty (fresh) store");
}

/* Scenario 5: an unknown (future) header version must be rejected, not misread
 * as a valid store. */
static void scenario_future_header(void)
{
    CHECK(write_store(99, 7, POWER_HISTORY_VERSION), "future-version store written");
    g_ready_line[0] = '\0';
    host_log_sink = log_sink;
    CHECK(power_history_init() == ESP_OK, "init ok after a future header");
    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.ready, "recovered store is ready (not the RAM fallback)");
    CHECK(st.record_count == 0, "an unsupported header version is not trusted");
}

/* Run a scenario in a fresh process: init() may run once per process and each
 * scenario needs its own file. The child's exit code carries its failure flag. */
static int run_scenario(const char *name, void (*fn)(void))
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        fflush(stdout);
        g_checks = 0;
        g_failures = 0; /* each scenario reports only its own checks/failures */
        fn();
        printf("  [%s] %d checks, %d failures\n", name, g_checks, g_failures);
        fflush(stdout);
        _exit(g_failures == 0 ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status)) {
        printf("  FAIL: scenario %s did not exit normally\n", name);
        return -1;
    }
    if (WEXITSTATUS(status) != 0) {
        printf("  FAIL: scenario %s reported failures\n", name);
        return -1;
    }
    return 0;
}

static void cleanup(void)
{
    remove(g_file_path);
    rmdir(POWER_HISTORY_MOUNT_PATH);
}

int main(void)
{
    /* Unbuffered stdout: the runner captures it through a pipe and the forked
     * scenario children must not re-emit the parent's buffered text. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (mkdir(POWER_HISTORY_MOUNT_PATH, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "could not create %s: %s\n",
                POWER_HISTORY_MOUNT_PATH, strerror(errno));
        return 2;
    }
    atexit(cleanup);

    printf("test_header_migration\n");
    CHECK(run_scenario("current", scenario_current) == 0,
          "current-version store boots with its records");
    CHECK(run_scenario("v2", scenario_old_v2) == 0,
          "v2 header/records boot with their records");
    CHECK(run_scenario("v1", scenario_old_v1) == 0,
          "v1 header/records boot with their records");
    CHECK(run_scenario("zero", scenario_zero_header) == 0,
          "all-zero header recovers to a fresh, ready store");
    CHECK(run_scenario("future", scenario_future_header) == 0,
          "unsupported header version is rejected");

    printf("  header-migration: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
