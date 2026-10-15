/* Does the exchange of missing data in the store work?
 *
 * The receiver mirrors the sender's minute store via the ESP-NOW backfill
 * (history_sync -> power_history_merge). When the sender was offline it writes
 * "boot marker" minutes (reason != 0) for the window it could not measure, so
 * the receiver can tell an expected outage apart from a real hole. This test
 * drives the real store (queue + writer task + plain-file fallback) to prove:
 *
 *   1. A marker minute is persisted and read back with its reason intact.
 *   2. The backfill merge pulls the marker across, reason preserved, and is
 *      idempotent (a re-pull of an already-held minute changes nothing).
 *   3. power_history_summarize_window() classifies a window as
 *      measured / expected(marker) / missing, and ONLY the missing minutes are
 *      real loss - the question the dashboard loss counter must answer.
 *
 * Same plain-file fallback as test_history_backfill_merge.c: the mount point and
 * history file are redirected under the build tmp dir by the runner.
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

static power_history_record_t make_record(uint32_t seq, int64_t start_utc,
                                          int32_t mw, uint8_t reason)
{
    power_history_record_t r;
    memset(&r, 0, sizeof(r));
    r.sequence = seq;
    r.interval_start_utc = start_utc;
    r.average_milliwatts = mw;
    r.sample_count = reason == POWER_HISTORY_REASON_NONE ? 60 : 0;
    r.format_version = POWER_HISTORY_VERSION;
    r.reason = reason;
    r.checksum = crc_record(&r);
    return r;
}

static void wait_for_count(uint32_t expected)
{
    power_history_status_t st;
    for (int i = 0; i < 300; ++i) {
        power_history_get_status(&st);
        if (st.record_count == expected) {
            return;
        }
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

    printf("test_boot_marker_exchange\n");
    if (power_history_init() != ESP_OK) {
        fprintf(stderr, "init failed\n");
        return 3;
    }

    /* The sender's store after a restart: three measured minutes (base..base+2),
     * a four-minute offline gap it filled with BROWNOUT markers
     * (base+3..base+6), then live measured minutes again (base+7, base+8).
     * A batch is five records, so the first five flush promptly. */
    const int64_t base = 9000000000LL;
    power_history_record_t sender[10];
    sender[0] = make_record(1, base + 0 * 60, 1000, POWER_HISTORY_REASON_NONE);
    sender[1] = make_record(2, base + 1 * 60, 1100, POWER_HISTORY_REASON_NONE);
    sender[2] = make_record(3, base + 2 * 60, 1200, POWER_HISTORY_REASON_NONE);
    for (int i = 0; i < 4; ++i) {
        sender[3 + i] = make_record((uint32_t)(4 + i), base + (3 + i) * 60, 0,
                                    POWER_HISTORY_REASON_BROWNOUT);
    }
    sender[7] = make_record(8, base + 7 * 60, 1300, POWER_HISTORY_REASON_NONE);
    sender[8] = make_record(9, base + 8 * 60, 1400, POWER_HISTORY_REASON_NONE);
    sender[9] = make_record(10, base + 9 * 60, 1500, POWER_HISTORY_REASON_NONE);

    /* 1. The receiver pulls the whole block. Its own store is empty, so every
     *    record is new: the markers must arrive with their reason intact. */
    size_t merged = 0;
    CHECK(power_history_merge(sender, 10, &merged) == ESP_OK, "merge returns OK");
    CHECK(merged == 10, "all ten records merged");
    wait_for_count(10);
    power_history_status_t st;
    power_history_get_status(&st);
    CHECK(st.record_count == 10, "ten minutes stored");

    /* 2. Idempotent re-pull: the sync layer skips minutes the receiver already
     *    holds, but even a raw re-merge must not duplicate them. */
    CHECK(power_history_merge(sender, 10, &merged) == ESP_OK, "re-merge returns OK");
    usleep(200000);
    power_history_get_status(&st);
    CHECK(st.record_count == 10, "re-merge does not duplicate minutes");

    /* 3. The stored markers carry reason and zero samples (not a fake 0 W
     *    measurement); the measured minutes keep their values. */
    power_history_record_t page[16];
    size_t count = 0;
    bool overflow = false;
    CHECK(power_history_read(0, 0, page, 16, &count, &st, &overflow) == ESP_OK,
          "read back history");
    CHECK(count == 10, "ten minutes read back");
    int marker_seen = 0;
    int measured_seen = 0;
    for (size_t i = 0; i < count; ++i) {
        if (page[i].reason == POWER_HISTORY_REASON_BROWNOUT) {
            ++marker_seen;
            CHECK(page[i].sample_count == 0, "marker minute has no sample");
            CHECK(page[i].average_milliwatts == 0, "marker minute carries no power");
        } else if (page[i].reason == POWER_HISTORY_REASON_NONE) {
            ++measured_seen;
            CHECK(page[i].sample_count == 60, "measured minute keeps its samples");
        }
    }
    CHECK(marker_seen == 4, "four boot markers survived the exchange");
    CHECK(measured_seen == 6, "six measured minutes survived the exchange");

    /* 4. The window summary is the dashboard's answer. Over the full window the
     *    four offline minutes are "expected" (marker), the five are "measured",
     *    and nothing is "missing" - so zero real loss. */
    power_history_gap_summary_t summary;
    CHECK(power_history_summarize_window(base, base + 10 * 60, &summary) == ESP_OK,
          "summarize full window");
    CHECK(summary.total == 10, "window is ten minutes");
    CHECK(summary.measured == 6, "six measured minutes");
    CHECK(summary.expected == 4, "four expected (marker) minutes");
    CHECK(summary.missing == 0, "no real loss over the filled window");
    CHECK(summary.reasons[POWER_HISTORY_REASON_BROWNOUT] == 4,
          "all four expected minutes are attributed to the brownout");

    /* 5. A window with a genuine hole: extend past the sender's newest minute so
     *    two minutes have no record at all. Those are the only real loss. */
    CHECK(power_history_summarize_window(base, base + 12 * 60, &summary) == ESP_OK,
          "summarize window with a real hole");
    CHECK(summary.total == 12, "window is twelve minutes");
    CHECK(summary.present == 10, "ten minutes present");
    CHECK(summary.expected == 4, "markers still count as expected, not lost");
    CHECK(summary.missing == 2, "two minutes with no record are the real loss");

    printf("test_boot_marker_exchange: %d checks, %d failures\n", g_checks,
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
