#include "wall_clock.h"

#include <string.h>

/* Same CRC-32 (reflected 0xEDB88320) as the power-history records, so the two
 * on-disk formats are validated the same way. */
static uint32_t crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

bool wall_clock_utc_is_sane(int64_t utc_seconds)
{
    return utc_seconds >= WALL_CLOCK_MIN_UTC && utc_seconds <= WALL_CLOCK_MAX_UTC;
}

void wall_clock_make_record(wall_clock_record_t *record, int64_t utc_seconds)
{
    if (record == NULL) {
        return;
    }
    memset(record, 0, sizeof(*record));
    record->magic = WALL_CLOCK_MAGIC;
    record->version = WALL_CLOCK_VERSION;
    record->utc_seconds = utc_seconds;
    record->checksum = crc32((const uint8_t *)record,
                             offsetof(wall_clock_record_t, checksum));
}

bool wall_clock_record_is_valid(const wall_clock_record_t *record)
{
    if (record == NULL) {
        return false;
    }
    if (record->magic != WALL_CLOCK_MAGIC || record->version != WALL_CLOCK_VERSION) {
        return false;
    }
    if (!wall_clock_utc_is_sane(record->utc_seconds)) {
        return false;
    }
    return record->checksum ==
           crc32((const uint8_t *)record, offsetof(wall_clock_record_t, checksum));
}

int64_t wall_clock_offset_us_from_utc(int64_t utc_seconds, uint64_t uptime_ms)
{
    return utc_seconds * 1000000LL - (int64_t)uptime_ms * 1000LL;
}

int64_t wall_clock_utc_ms_from_offset(int64_t offset_us, uint64_t uptime_ms)
{
    return offset_us / 1000LL + (int64_t)uptime_ms;
}

int64_t wall_clock_bucket_start_utc(int64_t utc_seconds, int64_t interval_seconds)
{
    if (interval_seconds <= 0) {
        return utc_seconds;
    }
    int64_t remainder = utc_seconds % interval_seconds;
    if (remainder < 0) {
        remainder += interval_seconds;
    }
    return utc_seconds - remainder;
}

bool wall_clock_persist_due(int64_t last_saved_utc, int64_t now_utc,
                            int64_t interval_seconds)
{
    if (last_saved_utc == 0) {
        return true;
    }
    if (interval_seconds <= 0) {
        return now_utc != last_saved_utc;
    }
    int64_t delta = now_utc - last_saved_utc;
    if (delta < 0) {
        return true; /* a sync moved the clock backwards: store the correction */
    }
    return delta >= interval_seconds;
}
