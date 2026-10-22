/* WiFi RSSI log (sender): the pure due/period logic and the sample ring must
 * not sample early, must survive a backwards clock, must keep min/max/average
 * over valid samples only, and must roll the ring instead of overflowing. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "wifi_rssi_log.h"

static int g_checks;
static int g_failures;
#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

#define PERIOD_MS 60000u

int main(void)
{
    /* Nothing is due before the first period has elapsed. */
    wifi_rssi_log_t log;
    wifi_rssi_log_init(&log, PERIOD_MS, 1000);
    CHECK(!wifi_rssi_log_due(&log, 1000), "not due at init");
    CHECK(!wifi_rssi_log_due(&log, 1000 + PERIOD_MS - 1), "one ms short -> not due");
    CHECK(wifi_rssi_log_due(&log, 1000 + PERIOD_MS), "period elapsed -> due");

    /* Recording a sample clears due and advances the next period. */
    wifi_rssi_log_add(&log, -55, true, 1000 + PERIOD_MS);
    CHECK(!wifi_rssi_log_due(&log, 1000 + PERIOD_MS), "just sampled -> not due");
    CHECK(!wifi_rssi_log_due(&log, 1000 + 2 * PERIOD_MS - 1), "not yet a full period");
    CHECK(wifi_rssi_log_due(&log, 1000 + 2 * PERIOD_MS), "next period due");

    /* A backwards clock must not report due (it would sample in a loop). */
    wifi_rssi_log_add(&log, -60, true, 500000);
    CHECK(!wifi_rssi_log_due(&log, 400000), "backwards clock -> not due");
    CHECK(wifi_rssi_log_due(&log, 500000 + PERIOD_MS), "catches up after the skew");

    /* min/max/average track the valid samples. */
    wifi_rssi_log_t stats;
    wifi_rssi_log_init(&stats, PERIOD_MS, 0);
    wifi_rssi_log_add(&stats, -40, true, PERIOD_MS);
    wifi_rssi_log_add(&stats, -70, true, 2 * PERIOD_MS);
    wifi_rssi_log_add(&stats, -55, true, 3 * PERIOD_MS);
    CHECK(stats.valid_count == 3, "three valid samples counted");
    CHECK(stats.latest_dbm == -55, "latest sample kept");
    CHECK(stats.min_dbm == -70, "minimum tracked");
    CHECK(stats.max_dbm == -40, "maximum tracked");
    CHECK(wifi_rssi_log_average_dbm(&stats) == -55, "average of -40,-70,-55 is -55");

    /* A miss advances the period and counts, but does not touch the stats. */
    wifi_rssi_log_add(&stats, 0, false, 4 * PERIOD_MS);
    CHECK(stats.miss_count == 1, "miss counted");
    CHECK(stats.valid_count == 3, "miss does not add a valid sample");
    CHECK(stats.latest_dbm == -55, "miss leaves the latest value alone");
    CHECK(!wifi_rssi_log_due(&stats, 4 * PERIOD_MS), "miss still advances the period");

    /* The ring rolls: capacity + 10 samples leave exactly `capacity` entries. */
    wifi_rssi_log_t ring;
    wifi_rssi_log_init(&ring, PERIOD_MS, 0);
    for (uint32_t i = 0; i < WIFI_RSSI_LOG_CAPACITY + 10u; ++i) {
        wifi_rssi_log_add(&ring, (int8_t)(-50 - (int)(i % 20u)), true,
                          (uint64_t)(i + 1u) * PERIOD_MS);
    }
    CHECK(ring.count == WIFI_RSSI_LOG_CAPACITY, "ring capped at capacity");
    CHECK(ring.valid_count == WIFI_RSSI_LOG_CAPACITY + 10u,
          "session counter keeps growing past the ring");

    /* Zero period disables sampling entirely. */
    wifi_rssi_log_t off;
    wifi_rssi_log_init(&off, 0, 0);
    CHECK(!wifi_rssi_log_due(&off, 10u * PERIOD_MS), "zero period -> never due");
    CHECK(wifi_rssi_log_average_dbm(&off) == 0, "no samples -> average 0");

    printf("wifi rssi log: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
