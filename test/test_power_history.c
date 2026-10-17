/* Host tests for the pure power-history aggregation helpers.
 *
 * These exercise main/power_history.c's aggregation core directly (no hardware,
 * no SPIFFS, no NVS, no Wi-Fi). They are the machine-checked part of the
 * "15 minute averages / data gaps / day boundaries" requirements; the on-device
 * HTTP paths cannot be tested here and are marked as not tested in the report.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "power_history.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        ++g_checks;                                                 \
        if (!(cond)) {                                              \
            ++g_failures;                                           \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
        }                                                           \
    } while (0)

/* Same CRC as record_checksum() in power_history.c, so the test can build
 * records the production validator accepts. */
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

static void make_record(power_history_record_t *r, uint32_t seq, int64_t ts,
                        int32_t milliwatts, uint32_t samples)
{
    power_history_record_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.sequence = seq;
    tmp.interval_start_utc = ts;
    tmp.average_milliwatts = milliwatts;
    tmp.sample_count = samples;
    tmp.format_version = POWER_HISTORY_VERSION;
    tmp.reason = POWER_HISTORY_REASON_NONE;
    tmp.checksum = crc_record(&tmp);
    *r = tmp;
}

/* Same as make_record() but with an explicit on-disk format version, so the
 * migration path can be exercised with records written by an older firmware. */
static void make_record_versioned(power_history_record_t *r, uint32_t seq, int64_t ts,
                                  int32_t milliwatts, uint32_t samples, uint16_t version)
{
    make_record(r, seq, ts, milliwatts, samples);
    r->format_version = version;
    r->checksum = crc_record(r);
}

static int64_t day_start_utc(int year, int month, int day)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    return (int64_t)timegm(&tmv);
}

/* 1) A 15 minute bucket averages the 60 second records inside it. */
static void test_15min_average_basic(void)
{
    printf("test_15min_average_basic\n");
    int64_t window = 1000000000LL;
    const size_t buckets = 96;
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];
    power_history_record_t recs[30];

    for (int i = 0; i < 15; ++i) {
        make_record(&recs[i], (uint32_t)(i + 1), window + i * 60, 1000, 60);
    }
    for (int i = 0; i < 15; ++i) {
        make_record(&recs[15 + i], (uint32_t)(16 + i), window + 900 + i * 60,
                    2000, 60);
    }
    power_history_aggregate_init(window, 900, sum, count, starts, buckets);
    CHECK(power_history_aggregate_core(window, 900, recs, 30, sum, count,
                                       buckets) == ESP_OK, "core returns OK");
    CHECK(count[0] == 15, "bucket 0 has 15 one-minute samples");
    CHECK(fabs(sum[0] - 15.0) < 1e-9, "bucket 0 sums 15 W (15 * 1 W)");
    CHECK(count[1] == 15, "bucket 1 has 15 one-minute samples");
    CHECK(fabs(sum[1] - 30.0) < 1e-9, "bucket 1 sums 30 W (15 * 2 W)");
    CHECK(starts[1] == (window + 900) * 1000LL, "bucket 1 start is +900 s");
}

/* 2) A gap must stay a gap: an empty bucket has count 0 and is distinguishable
 *    from a genuine 0 W measurement (which has count > 0 and sum 0). */
static void test_gap_not_zero(void)
{
    printf("test_gap_not_zero\n");
    int64_t window = 2000000000LL;
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];
    power_history_record_t recs[3];

    make_record(&recs[0], 1, window, 5000, 60);          /* bucket 0 */
    make_record(&recs[1], 2, window + 2 * 900, 0, 60);   /* bucket 2, real 0 W */
    make_record(&recs[2], 3, window + 2 * 900 + 60, 0, 60);
    power_history_aggregate_init(window, 900, sum, count, starts, 96);
    CHECK(power_history_aggregate_core(window, 900, recs, 3, sum, count, 96) == ESP_OK,
          "core returns OK");
    CHECK(count[0] == 1, "bucket 0 present");
    CHECK(count[1] == 0, "bucket 1 is a gap (count 0)");
    CHECK(count[2] == 2, "bucket 2 present with genuine 0 W samples");
    CHECK(fabs(sum[2]) < 1e-9, "genuine 0 W sums to 0");
    /* A gap (count 0) and a measured 0 W (count 2) are therefore distinguishable. */
    CHECK(!(count[1] == count[2]), "gap and measured-zero differ by count");
}

/* 3) Bucket boundaries are exact: the last second of a bucket and the first
 *    second of the next land in different buckets. */
static void test_bucket_boundaries(void)
{
    printf("test_bucket_boundaries\n");
    int64_t window = 3000000000LL;
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];
    power_history_record_t recs[3];

    make_record(&recs[0], 1, window + 900 - 60, 1000, 60); /* last of bucket 0 */
    make_record(&recs[1], 2, window + 900, 2000, 60);      /* first of bucket 1 */
    make_record(&recs[2], 3, window + 86400 - 60, 3000, 60); /* last of bucket 95 */
    power_history_aggregate_init(window, 900, sum, count, starts, 96);
    CHECK(power_history_aggregate_core(window, 900, recs, 3, sum, count, 96) == ESP_OK,
          "core returns OK");
    CHECK(count[0] == 1, "record at 840 s is in bucket 0");
    CHECK(count[1] == 1, "record at 900 s is in bucket 1");
    CHECK(count[95] == 1, "record at 86340 s is in bucket 95");
}

/* 4) Records outside the window are ignored (before start, at/after end). */
static void test_out_of_window_ignored(void)
{
    printf("test_out_of_window_ignored\n");
    int64_t window = 4000000000LL;
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];
    power_history_record_t recs[3];

    make_record(&recs[0], 1, window - 60, 9999, 60);      /* before window */
    make_record(&recs[1], 2, window + 86400, 9999, 60);   /* exactly end */
    make_record(&recs[2], 3, window + 86400 + 60, 9999, 60); /* after end */
    power_history_aggregate_init(window, 900, sum, count, starts, 96);
    CHECK(power_history_aggregate_core(window, 900, recs, 3, sum, count, 96) == ESP_OK,
          "core returns OK");
    uint32_t total = 0;
    for (int i = 0; i < 96; ++i) {
        total += count[i];
    }
    CHECK(total == 0, "no out-of-window record is aggregated");
}

/* 5) Corrupt or foreign-format records are skipped, never silently accepted. */
static void test_invalid_records_skipped(void)
{
    printf("test_invalid_records_skipped\n");
    int64_t window = 5000000000LL;
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];
    power_history_record_t recs[3];

    make_record(&recs[0], 1, window, 1000, 60);
    make_record(&recs[1], 2, window + 60, 1000, 60);
    recs[1].checksum ^= 0xDEADBEEFu; /* corrupt */
    make_record(&recs[2], 3, window + 120, 1000, 60);
    recs[2].format_version = 99; /* wrong version */
    power_history_aggregate_init(window, 900, sum, count, starts, 96);
    CHECK(power_history_aggregate_core(window, 900, recs, 3, sum, count, 96) == ESP_OK,
          "core returns OK");
    CHECK(count[0] == 1, "only the valid record is aggregated");
    CHECK(fabs(sum[0] - 1.0) < 1e-9, "corrupt/wrong-version values excluded");
}

/* 6) A whole UTC day fills all 96 fifteen-minute buckets. */
static void test_full_day_96_buckets(void)
{
    printf("test_full_day_96_buckets\n");
    int64_t window = day_start_utc(2024, 5, 15);
    const size_t buckets = 96;
    static double sum[96];
    static uint32_t count[96];
    int64_t starts[96];
    static power_history_record_t recs[96 * 15];

    size_t n = 0;
    for (size_t b = 0; b < buckets; ++b) {
        for (int m = 0; m < 15; ++m) {
            make_record(&recs[n], (uint32_t)(n + 1),
                        window + (int64_t)b * 900 + m * 60, 1000, 60);
            ++n;
        }
    }
    power_history_aggregate_init(window, 900, sum, count, starts, buckets);
    CHECK(power_history_aggregate_core(window, 900, recs, n, sum, count, buckets) == ESP_OK,
          "core returns OK");
    int filled = 0;
    for (size_t b = 0; b < buckets; ++b) {
        if (count[b] == 15) {
            ++filled;
        }
    }
    CHECK(filled == 96, "all 96 buckets hold 15 samples");
}

/* 7) Leap-day boundary: 2024-02-29 has 96 buckets and 23:45 is the last one. */
static void test_leap_day_boundary(void)
{
    printf("test_leap_day_boundary\n");
    int64_t window = day_start_utc(2024, 2, 29);
    int64_t next = day_start_utc(2024, 3, 1);
    CHECK(next - window == 86400, "leap day 2024-02-29 is exactly 86400 s");
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];
    power_history_record_t recs[2];
    make_record(&recs[0], 1, window + 23 * 3600 + 45 * 60, 1000, 60); /* 23:45 */
    make_record(&recs[1], 2, next, 1000, 60);                          /* next day */
    power_history_aggregate_init(window, 900, sum, count, starts, 96);
    CHECK(power_history_aggregate_core(window, 900, recs, 2, sum, count, 96) == ESP_OK,
          "core returns OK");
    CHECK(count[95] == 1, "23:45 lands in bucket 95");
    uint32_t total = 0;
    for (int i = 0; i < 96; ++i) {
        total += count[i];
    }
    CHECK(total == 1, "next-day 00:00 is outside the window");
}

/* 8) UTC days are DST-neutral: the European DST switch days still have exactly
 *    96 buckets and are 86400 s long, because history timestamps are UTC. */
static void test_dst_neutral_utc_days(void)
{
    printf("test_dst_neutral_utc_days\n");
    int64_t spring = day_start_utc(2024, 3, 31); /* EU spring-forward day */
    int64_t autumn = day_start_utc(2024, 10, 27); /* EU fall-back day */
    int64_t day = 86400;
    CHECK(autumn - spring > 0, "day starts are ordered");
    /* Each UTC day is exactly 24 h regardless of local DST. */
    CHECK((day_start_utc(2024, 3, 31) + day) == day_start_utc(2024, 4, 1),
          "spring-forward day is 24 h in UTC");
    CHECK((day_start_utc(2024, 10, 27) + day) == day_start_utc(2024, 10, 28),
          "fall-back day is 24 h in UTC");
    CHECK((day * 1000) / 900000 == 96, "a UTC day has 96 fifteen-minute buckets");
}

/* 9) Regression: a freshly created header must be valid, and an all-zero
 *    header must be invalid. The latter guards the reboot-data-loss bug where
 *    the empty-file branch wrote magic=0, so the next boot saw two invalid
 *    copies and silently fell back to RAM (history reset to 0 every reboot). */
static void test_fresh_header_validity(void)
{
    printf("test_fresh_header_validity\n");
    power_history_header_t header;
    power_history_make_fresh_header(&header);
    CHECK(power_history_header_is_valid(&header), "fresh header is valid");
    CHECK(header.magic != 0, "fresh header carries a magic");
    CHECK(header.version == POWER_HISTORY_VERSION, "fresh header carries the current version");

    power_history_header_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    CHECK(!power_history_header_is_valid(&zeroed),
          "all-zero header (old bug output) is rejected");

    power_history_header_t wrong_version = header;
    wrong_version.version = 99;
    wrong_version.checksum = 0; /* checksum no longer matches either */
    CHECK(!power_history_header_is_valid(&wrong_version),
          "header with foreign version is rejected");
}

/* 10) Persistence migration: a format-version bump must not drop the stored
 *     history. A v1 record (generation stored in `reserved`) is accepted and
 *     upgraded in place, a current-version record is passed through unchanged,
 *     and a genuinely corrupt CRC is still rejected (strict across versions). */
static void test_record_migration(void)
{
    printf("test_record_migration\n");
    int64_t ts = 6000000000LL;

    /* v1 record: `reserved` carries the old slot generation. */
    power_history_record_t old_rec;
    make_record_versioned(&old_rec, 7, ts, 1500, 60, 1);
    old_rec.reserved = 0x12u;
    old_rec.checksum = crc_record(&old_rec);
    power_history_record_t copy = old_rec;
    CHECK(power_history_migrate_record(&copy), "v1 record migrates");
    CHECK(copy.format_version == POWER_HISTORY_VERSION,
          "migrated record carries the current version");
    CHECK(copy.reserved == 0, "v1 leftover generation is normalized to 0");
    CHECK(copy.reason == POWER_HISTORY_REASON_NONE,
          "v1 record becomes a measured minute");
    CHECK(copy.sequence == 7 && copy.interval_start_utc == ts &&
          copy.average_milliwatts == 1500 && copy.sample_count == 60,
          "value fields survive the migration");
    CHECK(copy.checksum == crc_record(&copy), "checksum is recomputed after migration");

    /* v2 record (no reason field yet) migrates to a measured v3 minute. */
    power_history_record_t v2_rec;
    make_record_versioned(&v2_rec, 9, ts + 120, 1750, 60, 2);
    v2_rec.reason = 0;
    v2_rec.checksum = crc_record(&v2_rec);
    power_history_record_t v2_copy = v2_rec;
    CHECK(power_history_migrate_record(&v2_copy), "v2 record migrates");
    CHECK(v2_copy.format_version == POWER_HISTORY_VERSION &&
          v2_copy.reason == POWER_HISTORY_REASON_NONE,
          "v2 -> v3 record becomes a measured minute");

    /* Current-version record is idempotent. */
    power_history_record_t fresh_rec;
    make_record(&fresh_rec, 8, ts + 60, 2000, 60);
    power_history_record_t fresh_copy = fresh_rec;
    CHECK(power_history_migrate_record(&fresh_copy), "current record is accepted");
    CHECK(fresh_copy.checksum == fresh_rec.checksum,
          "current record is left byte-identical");

    /* A real CRC mismatch is corruption, not a migration. */
    power_history_record_t corrupt = old_rec;
    corrupt.checksum ^= 0xDEADBEEFu;
    CHECK(!power_history_migrate_record(&corrupt), "CRC mismatch is rejected");

    /* Unknown (future) version is rejected rather than misread. */
    power_history_record_t future;
    make_record_versioned(&future, 9, ts, 1000, 60, 99);
    CHECK(!power_history_migrate_record(&future), "unknown version is rejected");

    /* Sequence 0 can never be a valid record. */
    power_history_record_t zero;
    make_record(&zero, 0, ts, 1000, 60);
    CHECK(!power_history_migrate_record(&zero), "sequence 0 is rejected");
}

/* 11) Aggregation keeps old-format data: a mixed v1/v3 set is fully averaged,
 *     not just the native records. */
static void test_migration_in_aggregation(void)
{
    printf("test_migration_in_aggregation\n");
    int64_t window = 7000000000LL;
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];
    power_history_record_t recs[2];

    make_record_versioned(&recs[0], 1, window, 1000, 60, 1); /* v1, 1 W */
    recs[0].reserved = 0xABu;
    recs[0].checksum = crc_record(&recs[0]);
    make_record(&recs[1], 2, window + 60, 3000, 60);         /* v3, 3 W */

    power_history_aggregate_init(window, 900, sum, count, starts, 96);
    CHECK(power_history_aggregate_core(window, 900, recs, 2, sum, count, 96) == ESP_OK,
          "core returns OK");
    CHECK(count[0] == 2, "both the v1 and the v3 record are aggregated");
    CHECK(fabs(sum[0] - 4.0) < 1e-9, "v1 1 W + v2 3 W = 4 W");
}

int main(void)
{
    test_15min_average_basic();
    test_gap_not_zero();
    test_bucket_boundaries();
    test_out_of_window_ignored();
    test_invalid_records_skipped();
    test_full_day_96_buckets();
    test_leap_day_boundary();
    test_dst_neutral_utc_days();
    test_fresh_header_validity();
    test_record_migration();
    test_migration_in_aggregation();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
