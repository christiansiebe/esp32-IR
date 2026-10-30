/* Host test for the persisted "last ESP-NOW channel" of the sender.
 *
 * Symptom it guards against: the sender learns its channel from the receiver's
 * SoftAP and kept it only in a RAM static, so every boot re-ran a discovery that
 * waits up to 30 s and, on a miss, pins the compile-time channel until the link
 * watchdog notices the silence. The fix persists the last good channel in NVS
 * and seeds the boot from it.
 *
 * Two halves:
 *   1. the pure record/validation (espnow_channel_store.c), and
 *   2. the real NVS store driven through a file-backed NVS shim, with the
 *      "reboot" done by forking a fresh process - the store lives in
 *      process-local statics, exactly like a real reset.
 *
 * Acceptance: write a channel, reboot, the sender starts on it; a corrupt or
 * out-of-range value is never seeded.
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
#include "espnow_channel_store.h"

/* --- file-backed NVS shim (implements test/host/include/nvs.h) ------------- */

#define NVS_MAX_OPEN 8
static char g_nvs_names[NVS_MAX_OPEN][32];
static bool g_nvs_used[NVS_MAX_OPEN];

static void nvs_path(char *out, size_t size, const char *name, const char *key)
{
    snprintf(out, size, "%s/%s.%s", ESPNOW_CHANNEL_TEST_DIR, name, key);
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

static void test_record_validation(void)
{
    printf("test_record_validation\n");
    CHECK(!espnow_channel_is_valid(0), "channel 0 (the not-set sentinel) is rejected");
    CHECK(espnow_channel_is_valid(1), "channel 1 is accepted");
    CHECK(espnow_channel_is_valid(13), "channel 13 is accepted");
    CHECK(!espnow_channel_is_valid(14), "channel 14 is rejected");

    espnow_channel_record_t record;
    espnow_channel_make_record(&record, 6);
    CHECK(espnow_channel_record_is_valid(&record), "a freshly made record is valid");
    CHECK(record.magic == ESPNOW_CHANNEL_MAGIC, "record carries the magic");
    CHECK(record.version == ESPNOW_CHANNEL_VERSION, "record carries the current version");
    CHECK(record.channel == 6, "record carries the channel");

    espnow_channel_record_t corrupt = record;
    corrupt.channel = 7; /* checksum no longer matches */
    CHECK(!espnow_channel_record_is_valid(&corrupt), "a tampered channel fails the checksum");

    espnow_channel_record_t out_of_range = record;
    espnow_channel_make_record(&out_of_range, 20);
    CHECK(!espnow_channel_record_is_valid(&out_of_range),
          "a record built with an out-of-range channel is rejected");

    espnow_channel_record_t wrong_version = record;
    wrong_version.version = ESPNOW_CHANNEL_VERSION + 1;
    CHECK(!espnow_channel_record_is_valid(&wrong_version), "a foreign version is rejected");

    CHECK(!espnow_channel_record_is_valid(NULL), "a NULL record is rejected");
}

static void test_store_round_trip(void)
{
    printf("test_store_round_trip\n");

    uint8_t channel = 99;
    bool found = true;
    CHECK(espnow_channel_store_load(&channel, &found) == ESP_OK && !found,
          "an empty store reports not-found");
    CHECK(channel == 0, "an empty store leaves the out value at 0");

    CHECK(espnow_channel_store_save(11) == ESP_OK, "save channel 11");
    CHECK(espnow_channel_store_load(&channel, &found) == ESP_OK && found,
          "the stored channel is found");
    CHECK(channel == 11, "the stored channel is returned");
}

static void test_store_rejects_bad_channel(void)
{
    printf("test_store_rejects_bad_channel\n");
    CHECK(espnow_channel_store_save(0) == ESP_ERR_INVALID_ARG,
          "saving the sentinel channel 0 is refused");
    CHECK(espnow_channel_store_save(14) == ESP_ERR_INVALID_ARG,
          "saving an out-of-range channel is refused");

    /* The refusal must not have overwritten the good value from the round-trip. */
    uint8_t channel = 0;
    bool found = false;
    CHECK(espnow_channel_store_load(&channel, &found) == ESP_OK && found && channel == 11,
          "a refused save leaves the previous value intact");
}

static void test_store_rejects_corruption(void)
{
    printf("test_store_rejects_corruption\n");
    nvs_handle_t handle;
    CHECK(nvs_open("espnowch", NVS_READWRITE, &handle) == ESP_OK, "open the namespace");
    uint8_t garbage[sizeof(espnow_channel_record_t)];
    memset(garbage, 0xA5, sizeof(garbage));
    CHECK(nvs_set_blob(handle, "last", garbage, sizeof(garbage)) == ESP_OK,
          "write a corrupt blob");
    nvs_commit(handle);
    nvs_close(handle);

    uint8_t channel = 9;
    bool found = true;
    CHECK(espnow_channel_store_load(&channel, &found) == ESP_OK && !found,
          "a corrupt stored channel is ignored, not seeded");
    CHECK(channel == 0, "a corrupt store leaves the out value at 0");
}

/* The reboot: a forked child starts from a clean address space, so it can only
 * see the channel the parent persisted - exactly like a reset. */
static void test_channel_survives_reboot(void)
{
    printf("test_channel_survives_reboot\n");
    CHECK(espnow_channel_store_save(11) == ESP_OK, "persist channel 11 before the reboot");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        ++g_failures;
        return;
    }
    if (pid == 0) {
        uint8_t channel = 0;
        bool found = false;
        if (espnow_channel_store_load(&channel, &found) != ESP_OK || !found) {
            fprintf(stderr, "child: stored channel not found after reboot\n");
            _exit(1);
        }
        _exit(channel == 11 ? 0 : 2);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the channel is restored after a reboot");
}

static void cleanup(void)
{
    char path[128];
    nvs_path(path, sizeof(path), "espnowch", "last");
    remove(path);
    rmdir(ESPNOW_CHANNEL_TEST_DIR);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mkdir(ESPNOW_CHANNEL_TEST_DIR, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "could not create %s: %s\n", ESPNOW_CHANNEL_TEST_DIR, strerror(errno));
        return 2;
    }
    atexit(cleanup);

    test_record_validation();
    test_store_round_trip();
    test_store_rejects_bad_channel();
    test_store_rejects_corruption();
    test_channel_survives_reboot();

    printf("  espnow-channel: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
