#include "history_csv_import.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Civil date -> days since 1970-01-01 (Howard Hinnant's algorithm). Used instead
 * of timegm()/mktime(), which are not portable across host and newlib. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool parse_iso_utc(const char *text, int64_t *out)
{
    int year, month, day, hour, minute, second;
    char tail = '\0';
    int fields = sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%c",
                        &year, &month, &day, &hour, &minute, &second, &tail);
    if (fields < 6) {
        return false;
    }
    if (fields == 7 && tail != 'Z') {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return false;
    }
    *out = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
           hour * 3600 + minute * 60 + second;
    return true;
}

static bool field(const char *line, size_t len, int index, char *out, size_t out_size)
{
    size_t start = 0;
    int current = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || line[i] == ',') {
            if (current == index) {
                size_t begin = start;
                size_t end = i;
                while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) {
                    ++begin;
                }
                while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                                       line[end - 1] == '\r' || line[end - 1] == '\n')) {
                    --end;
                }
                size_t n = end - begin;
                if (n >= out_size) {
                    n = out_size - 1;
                }
                memcpy(out, line + begin, n);
                out[n] = '\0';
                return true;
            }
            ++current;
            start = i + 1;
        }
    }
    return false;
}

static uint8_t reason_from_token(const char *token)
{
    for (uint8_t i = 0; i < POWER_HISTORY_REASON_COUNT; ++i) {
        if (strcmp(token, power_history_reason_token(i)) == 0) {
            return i;
        }
    }
    return POWER_HISTORY_REASON_UNKNOWN;
}

static int32_t milliwatts_from_watts(double watts)
{
    double scaled = watts * 1000.0;
    return (int32_t)(scaled + (scaled < 0 ? -0.5 : 0.5));
}

static bool parse_line(const char *line, size_t len, power_history_record_t *record)
{
    char timestamp[32];
    char watts[32];
    char samples[32];
    char reason[24];

    if (!field(line, len, 1, timestamp, sizeof(timestamp)) ||
        !field(line, len, 2, watts, sizeof(watts)) ||
        !field(line, len, 3, samples, sizeof(samples))) {
        return false;
    }
    int64_t minute;
    if (!parse_iso_utc(timestamp, &minute)) {
        return false;
    }
    /* Reject a pre-2024 timestamp (1970 epoch / zero, i.e. a device that never
     * had a wall clock): it is not a real measurement and would sort before
     * every stored minute. Counted malformed by the caller. */
    if (minute < POWER_HISTORY_MIN_VALID_UTC) {
        return false;
    }
    char *end = NULL;
    double average_w = strtod(watts, &end);
    if (end == watts) {
        return false;
    }
    unsigned long sample_count = strtoul(samples, &end, 10);
    if (end == samples) {
        return false;
    }

    memset(record, 0, sizeof(*record));
    record->interval_start_utc = minute;
    record->average_milliwatts = milliwatts_from_watts(average_w);
    record->sample_count = (uint32_t)sample_count;
    record->format_version = POWER_HISTORY_VERSION;
    record->reason = POWER_HISTORY_REASON_NONE;
    if (field(line, len, 4, reason, sizeof(reason)) && reason[0] != '\0') {
        record->reason = reason_from_token(reason);
    }
    return true;
}

static int compare_records(const void *a, const void *b)
{
    const power_history_record_t *ra = a;
    const power_history_record_t *rb = b;
    if (ra->interval_start_utc < rb->interval_start_utc) {
        return -1;
    }
    if (ra->interval_start_utc > rb->interval_start_utc) {
        return 1;
    }
    return 0;
}

void history_csv_sort(power_history_record_t *records, size_t count)
{
    if (records != NULL && count > 1) {
        qsort(records, count, sizeof(records[0]), compare_records);
    }
}

bool history_csv_parse_line(const char *line, size_t len,
                            power_history_record_t *record)
{
    if (line == NULL || record == NULL) {
        return false;
    }
    return parse_line(line, len, record);
}

size_t history_csv_parse(const char *text, size_t len,
                         power_history_record_t *out, size_t capacity,
                         size_t *skipped_out)
{
    size_t written = 0;
    size_t skipped = 0;
    if (skipped_out != NULL) {
        *skipped_out = 0;
    }
    if (text == NULL || out == NULL || capacity == 0) {
        return 0;
    }

    bool seen_line = false;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') {
            ++j;
        }
        const char *line = text + i;
        size_t line_len = j - i;

        bool blank = true;
        for (size_t k = 0; k < line_len; ++k) {
            if (line[k] != ' ' && line[k] != '\t' && line[k] != '\r') {
                blank = false;
                break;
            }
        }
        if (!blank) {
            power_history_record_t record;
            if (parse_line(line, line_len, &record)) {
                if (written < capacity) {
                    out[written++] = record;
                } else {
                    ++skipped;
                }
            } else if (seen_line) {
                ++skipped;
            }
            /* A failed first line is treated as the optional header. */
            seen_line = true;
        }
        i = j + 1;
    }

    if (skipped_out != NULL) {
        *skipped_out = skipped;
    }
    return written;
}
