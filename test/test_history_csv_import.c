/* Host tests for the pure CSV restore parser (main/history_csv_import.c).
 *
 * The parser backs the receiver's "restore lost minutes from a backup" upload,
 * so it must accept exactly what /api/power-history.csv exports and reject
 * anything it cannot trust, without touching SPIFFS or ESP-IDF.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "history_csv_import.h"

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

static size_t parse(const char *text, power_history_record_t *out, size_t cap,
                    size_t *skipped)
{
    return history_csv_parse(text, strlen(text), out, cap, skipped);
}

static void test_header_and_rows(void)
{
    const char *csv =
        "sequence,interval_start_ms,average_w,sample_count,reason\r\n"
        "1,2026-10-13T09:00:00Z,123.456,60,none\r\n"
        "2,2026-10-13T09:01:00Z,-42.5,60,crash\r\n";
    power_history_record_t records[8];
    size_t skipped = 0;
    size_t n = parse(csv, records, 8, &skipped);
    CHECK(n == 2, "header + two rows -> two records");
    CHECK(skipped == 0, "no skipped rows");
    CHECK(records[0].interval_start_utc == 1791882000LL, "first minute epoch");
    CHECK(records[1].interval_start_utc == 1791882060LL, "second minute epoch");
    CHECK(records[0].average_milliwatts == 123456, "watts -> milliwatts rounding");
    CHECK(records[1].average_milliwatts == -42500, "negative watts preserved");
    CHECK(records[0].sample_count == 60, "sample_count parsed");
    CHECK(records[0].reason == POWER_HISTORY_REASON_NONE, "reason none");
    CHECK(records[1].reason == POWER_HISTORY_REASON_CRASH, "reason crash");
    CHECK(records[0].format_version == POWER_HISTORY_VERSION, "current format version");
    CHECK(records[0].sequence == 0, "sequence left for the store to assign");
}

static void test_no_header_lf(void)
{
    const char *csv = "1,2026-10-13T09:00:00Z,10,60,none\n";
    power_history_record_t records[4];
    size_t skipped = 0;
    size_t n = parse(csv, records, 4, &skipped);
    CHECK(n == 1, "LF without header parses");
    CHECK(skipped == 0, "no skipped");
}

static void test_reason_optional(void)
{
    const char *csv = "9,2026-10-13T09:00:00Z,5,60\n";
    power_history_record_t records[4];
    size_t skipped = 0;
    size_t n = parse(csv, records, 4, &skipped);
    CHECK(n == 1, "missing reason column accepted");
    CHECK(records[0].reason == POWER_HISTORY_REASON_NONE, "missing reason defaults to none");
}

static void test_malformed_rows_skipped(void)
{
    const char *csv =
        "sequence,interval_start_ms,average_w,sample_count,reason\n"
        "1,2026-10-13T09:00:00Z,10,60,none\n"
        "2,not-a-timestamp,10,60,none\n"
        "3,2026-10-13T09:02:00Z,not-a-number,60,none\n"
        "4,2026-10-13T09:03:00Z,10,60,none\n";
    power_history_record_t records[8];
    size_t skipped = 0;
    size_t n = parse(csv, records, 8, &skipped);
    CHECK(n == 2, "only the well-formed rows survive");
    CHECK(skipped == 2, "both malformed rows counted");
    CHECK(records[0].interval_start_utc == 1791882000LL, "row 1 kept");
    CHECK(records[1].interval_start_utc == 1791882180LL, "row 4 kept");
}

static void test_capacity_limit(void)
{
    const char *csv =
        "1,2026-10-13T09:00:00Z,10,60,none\n"
        "2,2026-10-13T09:01:00Z,10,60,none\n"
        "3,2026-10-13T09:02:00Z,10,60,none\n";
    power_history_record_t records[2];
    size_t skipped = 0;
    size_t n = parse(csv, records, 2, &skipped);
    CHECK(n == 2, "capacity honoured");
    CHECK(skipped == 1, "overflow row counted as skipped");
}

static void test_epoch_edge_cases(void)
{
    const char *csv =
        "1,1970-01-01T00:00:00Z,0,0,none\n"
        "2,2026-02-28T23:59:00Z,1,1,none\n"
        "3,2028-02-29T12:00:00Z,1,1,none\n";
    power_history_record_t records[4];
    size_t skipped = 0;
    size_t n = parse(csv, records, 4, &skipped);
    CHECK(n == 3, "edge timestamps parse");
    CHECK(records[0].interval_start_utc == 0LL, "epoch zero");
    CHECK(records[1].interval_start_utc == 1772323140LL, "non-leap end of February");
    CHECK(records[2].interval_start_utc == 1835438400LL, "leap day 2028");
}

static void test_sort(void)
{
    power_history_record_t records[3];
    memset(records, 0, sizeof(records));
    records[0].interval_start_utc = 300;
    records[1].interval_start_utc = 100;
    records[2].interval_start_utc = 200;
    history_csv_sort(records, 3);
    CHECK(records[0].interval_start_utc == 100, "sorted first");
    CHECK(records[1].interval_start_utc == 200, "sorted second");
    CHECK(records[2].interval_start_utc == 300, "sorted third");
}

int main(void)
{
    test_header_and_rows();
    test_no_header_lf();
    test_reason_optional();
    test_malformed_rows_skipped();
    test_capacity_limit();
    test_epoch_edge_cases();
    test_sort();
    printf("history_csv_import: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
