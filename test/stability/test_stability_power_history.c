/* Stability / fault-injection harness for the power-history record migration,
 * the header validator and the aggregation core.
 *
 * Ten independent fault-injection tests. Unlike test_power_history.c they do not
 * check a single happy path but stress the pure logic with corrupted, truncated,
 * adversarial and boundary inputs, and must never crash, read out of bounds or
 * accept an inconsistent record. Built with ASan + UBSan.
 *
 * No hardware, no SPIFFS, no FreeRTOS task is started (the host shim's
 * xTaskCreate is a no-op), so everything here is deterministic and portable.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "power_history.h"

static int g_checks;
static int g_failures;
static int g_test_fails;

#define TSTART(name)                                    \
    do {                                                \
        printf("%-30s", (name));                        \
        g_test_fails = 0;                               \
    } while (0)
#define TEND()                                          \
    do {                                                \
        if (g_test_fails == 0) {                        \
            printf("PASS\n");                           \
        } else {                                        \
            printf("FAIL (%d)\n", g_test_fails);        \
        }                                               \
    } while (0)
#define CHECK(cond, msg)                                              \
    do {                                                              \
        ++g_checks;                                                   \
        if (!(cond)) {                                                \
            ++g_failures;                                             \
            ++g_test_fails;                                           \
            printf("\n    FAIL: %s (%s:%d)", msg, __FILE__, __LINE__);\
        }                                                             \
    } while (0)

static uint64_t g_rng = 0x123456789ABCDEF0ULL;
static uint64_t rng(void)
{
    uint64_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

/* Same CRC as record_checksum() in power_history.c. */
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
                        int32_t mw, uint32_t n, uint16_t version)
{
    memset(r, 0, sizeof(*r));
    r->sequence = seq;
    r->interval_start_utc = ts;
    r->average_milliwatts = mw;
    r->sample_count = n;
    r->format_version = version;
    r->checksum = crc_record(r);
}

/* Invariant that must hold whenever migrate() accepts a record. */
static bool record_consistent(const power_history_record_t *r)
{
    return r->sequence != 0 &&
           r->format_version >= POWER_HISTORY_OLDEST_SUPPORTED_VERSION &&
           r->format_version <= POWER_HISTORY_VERSION &&
           r->checksum == crc_record(r);
}

/* 1) Random bit flips in a valid record: never accepted with an inconsistent
 *    CRC/version, never crashes. At least some flips must be rejected. */
static void test_bitflips(void)
{
    TSTART("bitflips");
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 4000; ++i) {
        power_history_record_t r;
        uint16_t ver = (i & 1) ? 1u : 2u;
        make_record(&r, 42, 1700000000LL, 1234, 60, ver);
        if (ver == 1u) {
            r.reserved = 0x1234u;
            r.checksum = crc_record(&r);
        }
        int flips = 1 + (int)(rng() % 3u);
        for (int f = 0; f < flips; ++f) {
            uint32_t bit = (uint32_t)(rng() % (sizeof(r) * 8u));
            ((uint8_t *)&r)[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        }
        if (power_history_migrate_record(&r)) {
            ++accepted;
            CHECK(record_consistent(&r), "accepted record is CRC/version consistent");
        } else {
            ++rejected;
        }
    }
    CHECK(rejected > 0, "at least some bit flips are rejected");
    printf("  [accepted=%d rejected=%d] ", accepted, rejected);
    TEND();
}

/* 2) Fully random buffers (garbage/truncation): never accepted unless the CRC
 *    genuinely matches, never crashes. */
static void test_garbage(void)
{
    TSTART("garbage_buffers");
    int accepted = 0, rejected = 0;
    for (int i = 0; i < 4000; ++i) {
        power_history_record_t r;
        memset(&r, 0, sizeof(r));
        for (size_t b = 0; b < sizeof(r); ++b) {
            ((uint8_t *)&r)[b] = (uint8_t)(rng() & 0xFFu);
        }
        if (power_history_migrate_record(&r)) {
            ++accepted;
            CHECK(record_consistent(&r), "accepted garbage is CRC/version consistent");
        } else {
            ++rejected;
        }
    }
    CHECK(rejected > accepted, "random garbage is rejected far more often");
    printf("  [accepted=%d rejected=%d] ", accepted, rejected);
    TEND();
}

/* 3) Sequence and header boundary values. */
static void test_sequence_bounds(void)
{
    TSTART("sequence_bounds");
    power_history_record_t r;
    make_record(&r, 0, 0, 0, 0, 2);
    CHECK(!power_history_migrate_record(&r), "sequence 0 is rejected");
    make_record(&r, 1, 0, 0, 0, 2);
    CHECK(power_history_migrate_record(&r), "sequence 1 is accepted");
    make_record(&r, 0xFFFFFFFFu, 0, 0, 0, 2);
    CHECK(power_history_migrate_record(&r), "sequence UINT32_MAX is accepted");

    power_history_header_t h;
    power_history_make_fresh_header(&h);
    CHECK(power_history_header_is_valid(&h), "fresh header valid");
    power_history_header_t z = h;
    z.next_sequence = 0;
    z.checksum = 0;
    CHECK(!power_history_header_is_valid(&z), "next_sequence 0 rejected");
    z = h;
    z.next_slot = POWER_HISTORY_CAPACITY;
    z.checksum = 0;
    CHECK(!power_history_header_is_valid(&z), "next_slot >= capacity rejected");
    z = h;
    z.record_count = POWER_HISTORY_CAPACITY + 1u;
    z.checksum = 0;
    CHECK(!power_history_header_is_valid(&z), "record_count > capacity rejected");
    TEND();
}

/* 4) Exhaustive version matrix. */
static void test_version_matrix(void)
{
    TSTART("version_matrix");
    const uint16_t versions[] = {0u, 1u, 2u, 3u, 99u, 0xFFFFu};
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
        power_history_record_t r;
        make_record(&r, 5, 1000, 1000, 60, versions[i]);
        bool ok = power_history_migrate_record(&r);
        bool expect = versions[i] >= POWER_HISTORY_OLDEST_SUPPORTED_VERSION &&
                      versions[i] <= POWER_HISTORY_VERSION;
        CHECK(ok == expect, "version in-range accepted, out-of-range rejected");
        if (ok) {
            CHECK(r.format_version == POWER_HISTORY_VERSION, "migrated to current");
            CHECK(record_consistent(&r), "result consistent");
        }
    }
    TEND();
}

/* 5) Aggregation argument guards: every invalid combo returns INVALID_ARG. */
static void test_agg_bad_args(void)
{
    TSTART("aggregate_bad_args");
    double sum[4];
    uint32_t count[4];
    power_history_record_t rec;
    make_record(&rec, 1, 1000, 1000, 60, 2);
    CHECK(power_history_aggregate_core(0, 900, &rec, 1, NULL, count, 4) == ESP_ERR_INVALID_ARG,
          "NULL sum rejected");
    CHECK(power_history_aggregate_core(0, 900, &rec, 1, sum, NULL, 4) == ESP_ERR_INVALID_ARG,
          "NULL count rejected");
    CHECK(power_history_aggregate_core(0, 900, &rec, 1, sum, count, 0) == ESP_ERR_INVALID_ARG,
          "bucket_count 0 rejected");
    CHECK(power_history_aggregate_core(0, 0, &rec, 1, sum, count, 4) == ESP_ERR_INVALID_ARG,
          "bucket_seconds 0 rejected");
    CHECK(power_history_aggregate_core(0, -900, &rec, 1, sum, count, 4) == ESP_ERR_INVALID_ARG,
          "negative bucket_seconds rejected");
    CHECK(power_history_aggregate_core(0, 900, NULL, 1, sum, count, 4) == ESP_ERR_INVALID_ARG,
          "NULL records with count>0 rejected");
    CHECK(power_history_aggregate_core(0, 900, NULL, 0, sum, count, 4) == ESP_OK,
          "NULL records with count 0 accepted");
    TEND();
}

/* 6) Extreme timestamps and huge buckets: out-of-window and out-of-range records
 *    are ignored, in-window ones land in the right bucket, never crashes. */
static void test_agg_extreme(void)
{
    TSTART("aggregate_extreme");
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];

    int64_t window = 0;
    power_history_record_t recs[4];
    make_record(&recs[0], 1, 0, 1000, 60, 2);                 /* bucket 0 */
    make_record(&recs[1], 2, -1, 9999, 60, 2);                /* before window */
    make_record(&recs[2], 3, 1LL << 62, 9999, 60, 2);         /* far future */
    make_record(&recs[3], 4, 86400, 9999, 60, 2);             /* exactly end */
    power_history_aggregate_init(window, 900, sum, count, starts, 96);
    CHECK(power_history_aggregate_core(window, 900, recs, 4, sum, count, 96) == ESP_OK,
          "extreme ts: core OK");
    CHECK(count[0] == 1, "in-window record counted");
    uint32_t total = 0;
    for (int i = 0; i < 96; ++i) {
        total += count[i];
    }
    CHECK(total == 1, "out-of-window extremes ignored");

    /* Very large bucket: index arithmetic stays in range. */
    int64_t big = 1LL << 40;
    double s2[96];
    uint32_t c2[96];
    int64_t st2[96];
    power_history_record_t r2[2];
    make_record(&r2[0], 1, 5 * big, 1000, 60, 2);   /* idx 5 */
    make_record(&r2[1], 2, 1000LL * big, 1000, 60, 2); /* idx 1000 -> ignored */
    power_history_aggregate_init(0, big, s2, c2, st2, 96);
    CHECK(power_history_aggregate_core(0, big, r2, 2, s2, c2, 96) == ESP_OK,
          "huge bucket: core OK");
    CHECK(c2[5] == 1, "record at idx 5 placed");
    uint32_t t2 = 0;
    for (int i = 0; i < 96; ++i) {
        t2 += c2[i];
    }
    CHECK(t2 == 1, "out-of-range index ignored");
    TEND();
}

/* 7) Corrupt records mixed with valid ones: only valid records counted. */
static void test_agg_corrupt_mix(void)
{
    TSTART("aggregate_corrupt_mix");
    int64_t window = 1500000000LL;
    double sum[96];
    uint32_t count[96];
    int64_t starts[96];
    power_history_record_t recs[10];
    for (int i = 0; i < 5; ++i) {
        make_record(&recs[i], (uint32_t)(i + 1), window + i * 60, 1000, 60, 2);
    }
    make_record(&recs[5], 6, window + 300, 1000, 60, 2);
    recs[5].checksum ^= 0xDEADBEEFu; /* bad CRC */
    make_record(&recs[6], 7, window + 360, 1000, 60, 77); /* bad version */
    make_record(&recs[7], 8, window + 420, 1000, 60, 1); /* v1, valid -> migrate */
    recs[7].reserved = 0x55u;
    recs[7].checksum = crc_record(&recs[7]);
    make_record(&recs[8], 0, window + 480, 1000, 60, 2); /* seq 0 */
    make_record(&recs[9], 10, window + 540, 1000, 60, 2); /* valid v2 */

    power_history_aggregate_init(window, 900, sum, count, starts, 96);
    CHECK(power_history_aggregate_core(window, 900, recs, 10, sum, count, 96) == ESP_OK,
          "core OK");
    CHECK(count[0] == 7, "7 usable records (5 v2 + 1 v1 migrated + 1 more v2)");
    CHECK(fabs(sum[0] - 7.0) < 1e-9, "only valid values summed (7 x 1 W)");
    TEND();
}

/* 8) read() argument guards and not-ready contract. */
static void test_read_guards(void)
{
    TSTART("read_guards");
    power_history_record_t buf[4];
    size_t n = 0;
    bool over = false;
    power_history_status_t st;
    CHECK(power_history_read(0, 0, NULL, 4, &n, &st, &over) == ESP_ERR_INVALID_ARG,
          "NULL records rejected");
    CHECK(power_history_read(0, 0, buf, 0, &n, &st, &over) == ESP_ERR_INVALID_ARG,
          "capacity 0 rejected");
    CHECK(power_history_read(0, 0, buf, 4, NULL, &st, &over) == ESP_ERR_INVALID_ARG,
          "NULL count rejected");
    CHECK(power_history_read(0, 0, buf, 4, &n, NULL, &over) == ESP_ERR_INVALID_ARG,
          "NULL status rejected");
    CHECK(power_history_read(0, 0, buf, 4, &n, &st, NULL) == ESP_ERR_INVALID_ARG,
          "NULL overflow rejected");
    /* No init() was called: the history must report not-ready. */
    CHECK(power_history_read(0, 0, buf, 4, &n, &st, &over) == ESP_ERR_INVALID_STATE,
          "not-ready history reports INVALID_STATE");
    TEND();
}

/* 9) Header corruption: a flip in any covered byte must invalidate the header. */
static void test_header_corruption(void)
{
    TSTART("header_corruption");
    power_history_header_t good;
    power_history_make_fresh_header(&good);
    CHECK(power_history_header_is_valid(&good), "pristine header valid");
    int still_valid = 0;
    for (size_t b = 0; b < sizeof(good); ++b) {
        for (int bit = 0; bit < 8; ++bit) {
            power_history_header_t h = good;
            ((uint8_t *)&h)[b] ^= (uint8_t)(1u << bit);
            if (power_history_header_is_valid(&h)) {
                ++still_valid;
            }
        }
    }
    CHECK(still_valid == 0, "every single-bit header flip is rejected");
    TEND();
}

/* 10) Migration idempotence and on-disk layout stability. */
static void test_idempotent_layout(void)
{
    TSTART("idempotent_layout");
    CHECK(sizeof(power_history_record_t) == 28, "record struct is 28 bytes (packed)");
    CHECK(offsetof(power_history_record_t, checksum) == 24, "record checksum offset stable");
    CHECK(sizeof(power_history_header_t) == 28, "header struct is 28 bytes (packed)");
    CHECK(offsetof(power_history_header_t, checksum) == 24, "header checksum offset stable");

    power_history_record_t r;
    make_record(&r, 9, 1000, 2000, 60, 1);
    r.reserved = 0xBEu;
    r.checksum = crc_record(&r);
    CHECK(power_history_migrate_record(&r), "v1 migrates");
    power_history_record_t after_first = r;
    CHECK(power_history_migrate_record(&r), "second migration accepted");
    CHECK(memcmp(&after_first, &r, sizeof(r)) == 0, "migration is idempotent");

    /* A current-version (v3) record must be passed through untouched; a v2 one
     * is upgraded in place because the new `reason` field did not exist yet. */
    power_history_record_t v3;
    make_record(&v3, 10, 2000, 3000, 60, POWER_HISTORY_VERSION);
    power_history_record_t v3_before = v3;
    CHECK(power_history_migrate_record(&v3), "v3 accepted");
    CHECK(memcmp(&v3_before, &v3, sizeof(v3)) == 0, "v3 left byte-identical");

    power_history_record_t v2;
    make_record(&v2, 11, 3000, 4000, 60, 2);
    CHECK(power_history_migrate_record(&v2), "v2 accepted");
    CHECK(v2.format_version == POWER_HISTORY_VERSION &&
          v2.reason == POWER_HISTORY_REASON_NONE,
          "v2 upgraded to a measured v3 minute");
    TEND();
}

int main(void)
{
    test_bitflips();
    test_garbage();
    test_sequence_bounds();
    test_version_matrix();
    test_agg_bad_args();
    test_agg_extreme();
    test_agg_corrupt_mix();
    test_read_guards();
    test_header_corruption();
    test_idempotent_layout();

    printf("\nstability: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
