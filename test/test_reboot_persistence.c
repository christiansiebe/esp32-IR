/* Reboot-persistence host test for the power-history store.
 *
 * The other power-history host tests only exercise the pure logic: the host shim
 * never calls power_history_init(), so the storage path (open file, write a
 * record, read the header back) was untested. That is exactly where the
 * "record_count resets to 0 after reboot" bug lived. This test runs the real
 * init/write/status cycle and then simulates a reboot by forking a fresh process
 * that calls power_history_init() again and checks that record_count is restored.
 *
 * It uses the driver's plain-file fallback: esp_vfs_spiffs_register() reports
 * success and the store opens POWER_HISTORY_MOUNT_PATH "/quarters.bin" as an
 * ordinary file. The mount point is provisioned under the build/tmp directory
 * and removed afterwards; nothing outside the repo is touched.
 *
 * Child/parent results are exchanged through a pipe because the store's status
 * lives in process-local statics: a real reboot is a new process, not a function
 * call. On ESP-IDF this same sequence is what the on-device boot does.
 */
#include <errno.h>
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

/* The runner compiles both the store and this test with
 * POWER_HISTORY_MOUNT_PATH/POWER_HISTORY_FILE pointing under the build tmp dir,
 * so parent and every forked "reboot" child agree on the same scratch file. */
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

/* Child: boot once, write `count` records and exit. Must not touch SPIFFS in
 * the parent (globals are process-local), so this is the "first boot". */
static int write_phase(int count)
{
    if (power_history_init() != 0) {
        fprintf(stderr, "write_phase: init failed\n");
        return 10;
    }
    for (int i = 1; i <= count; ++i) {
        power_history_record_t r = make_record((uint32_t)i, 1000 + i);
        if (!power_history_enqueue(&r)) {
            fprintf(stderr, "write_phase: enqueue %d failed\n", i);
            return 11;
        }
    }
    /* The writer task persists asynchronously (in a real batch, after the
     * BATCH_TIMEOUT). Wait until the header reflects every record so the parent
     * fork does not race the write. */
    power_history_status_t st;
    for (int i = 0; i < 200; ++i) {
        power_history_get_status(&st);
        if (st.record_count == (uint32_t)count) {
            return 0;
        }
        usleep(10000);
    }
    fprintf(stderr, "write_phase: record_count=%u expected %d\n",
            (unsigned)st.record_count, count);
    return 12;
}

/* Child: reboot (fresh process image state) and report the restored count. */
static int reboot_phase(void)
{
    if (power_history_init() != 0) {
        fprintf(stderr, "reboot_phase: init failed\n");
        return 20;
    }
    power_history_status_t st;
    power_history_get_status(&st);
    if (!st.ready) {
        fprintf(stderr, "reboot_phase: not ready (RAM fallback)\n");
        return 21;
    }
    printf("%u\n", (unsigned)st.record_count);
    return 0;
}

static int run_write_child(int count)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        fflush(stdout);
        _exit(write_phase(count));
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/* Parent: fork a child that reports the restored count over a pipe. */
static int reboot_count(void)
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
    return atoi(buf);
}

static void cleanup(void)
{
    remove(POWER_HISTORY_FILE);
    rmdir(POWER_HISTORY_MOUNT_PATH);
}

int main(void)
{
    /* Unbuffered stdout: the runner captures it through a pipe (block
     * buffered), and the forked "reboot" children must not re-emit the parent's
     * buffered text instead of only their record count. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (mkdir(POWER_HISTORY_MOUNT_PATH, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "could not create %s: %s\n",
                POWER_HISTORY_MOUNT_PATH, strerror(errno));
        return 2;
    }
    atexit(cleanup);

    printf("test_reboot_restores_record_count\n");
    /* First boot: persist 3 minute records. */
    int rc = run_write_child(5);
    CHECK(rc == 0, "first boot writes and reports record_count==5");

    /* The on-disk file must now exist and be non-empty. */
    struct stat sb;
    CHECK(stat(g_file_path, &sb) == 0 && sb.st_size > 0,
          "persistent history file exists after first boot");

    /* Reboot: a fresh process must restore record_count from disk. */
    int restored = reboot_count();
    CHECK(restored == 5, "after reboot record_count is restored (not 0, no RAM fallback)");

    /* A second reboot must still report the same count (idempotent recovery). */
    int restored2 = reboot_count();
    CHECK(restored2 == 5, "a second reboot keeps record_count");

    printf("  reboot-persistence: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
