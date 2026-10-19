#include "energy_totals.h"

#include <stddef.h>
#include <string.h>

/* Same CRC-32 (reflected 0xEDB88320) as the power-history records and the wall
 * clock, so every on-disk format is validated the same way. */
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

/* Floor division/modulo that also works for negative values (a bogus clock must
 * not produce a negative remainder). */
static int64_t floor_div(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

/* Civil year/month/day for a day count since epoch (Howard Hinnant's algorithm).
 * Pure, so the calendar math is identical on the host and the device. */
static void civil_from_days(int64_t z, int64_t *year, unsigned int *month,
                            unsigned int *day)
{
    z += 719468;
    int64_t era = floor_div(z, 146097);
    unsigned int doe = (unsigned int)(z - era * 146097);              /* [0, 146096] */
    unsigned int yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned int doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);    /* [0, 365] */
    unsigned int mp = (5u * doy + 2u) / 153u;                         /* [0, 11] */
    unsigned int d = doy - (153u * mp + 2u) / 5u + 1u;               /* [1, 31] */
    unsigned int m = mp + (mp < 10u ? 3u : (unsigned int)-9);
    y += (m <= 2u);
    if (year != NULL) {
        *year = y;
    }
    if (month != NULL) {
        *month = m;
    }
    if (day != NULL) {
        *day = d;
    }
}

int64_t energy_totals_day_start_utc(int64_t utc_seconds)
{
    int64_t day = floor_div(utc_seconds, 86400);
    return day * 86400;
}

int32_t energy_totals_month_index(int64_t utc_seconds)
{
    int64_t day = floor_div(utc_seconds, 86400);
    int64_t year = 0;
    unsigned int month = 1;
    civil_from_days(day, &year, &month, NULL);
    return (int32_t)(year * 12 + (int64_t)(month - 1));
}

void energy_totals_reset(energy_totals_t *totals)
{
    if (totals == NULL) {
        return;
    }
    memset(totals, 0, sizeof(*totals));
}

/* Slot index for `key` in an array whose element's key field is at `key_offset`.
 * A free slot (key 0) is preferred, otherwise the oldest (smallest) key is
 * evicted. 0 is never a valid key here (dates are >= 2024, month indices are
 * large), so it safely marks an unused slot. */
static size_t slot_for(int64_t key, void *slots, size_t slot_count, size_t slot_size,
                       size_t key_offset)
{
    uint8_t *base = slots;
    size_t free_slot = SIZE_MAX;
    size_t oldest_slot = SIZE_MAX;
    int64_t oldest_key = 0;
    for (size_t i = 0; i < slot_count; ++i) {
        int64_t slot_key = 0;
        /* memcpy, not a cast: the slots are packed structs, so the key field may
         * be unaligned. */
        memcpy(&slot_key, base + i * slot_size + key_offset, sizeof(slot_key));
        if (slot_key == key) {
            return i;
        }
        if (slot_key == 0) {
            if (free_slot == SIZE_MAX) {
                free_slot = i;
            }
        } else if (oldest_slot == SIZE_MAX || slot_key < oldest_key) {
            oldest_slot = i;
            oldest_key = slot_key;
        }
    }
    return free_slot != SIZE_MAX ? free_slot : oldest_slot;
}

bool energy_totals_add(energy_totals_t *totals, const power_history_record_t *record)
{
    if (totals == NULL || record == NULL) {
        return false;
    }
    /* Only a real measurement counts. A boot marker (reason != NONE) or a record
     * without samples stands for an offline minute, not for zero consumption. */
    if (record->reason != POWER_HISTORY_REASON_NONE || record->sample_count == 0) {
        return false;
    }
    if (record->interval_start_utc < POWER_HISTORY_MIN_VALID_UTC) {
        return false;
    }

    int64_t day_start = energy_totals_day_start_utc(record->interval_start_utc);
    size_t day_slot = slot_for(day_start, totals->days, ENERGY_TOTALS_DAY_SLOTS,
                               sizeof(totals->days[0]), offsetof(energy_day_slot_t, day_utc));
    if (day_slot == SIZE_MAX) {
        return false;
    }
    if (totals->days[day_slot].day_utc != day_start) {
        memset(&totals->days[day_slot], 0, sizeof(totals->days[day_slot]));
        totals->days[day_slot].day_utc = day_start;
    }
    totals->days[day_slot].mw_minutes += record->average_milliwatts;
    ++totals->days[day_slot].minute_count;

    int64_t month_index = (int64_t)energy_totals_month_index(record->interval_start_utc);
    size_t month_slot = slot_for(month_index, totals->months, ENERGY_TOTALS_MONTH_SLOTS,
                                 sizeof(totals->months[0]),
                                 offsetof(energy_month_slot_t, month_index));
    if (month_slot != SIZE_MAX) {
        if (totals->months[month_slot].month_index != (int32_t)month_index) {
            memset(&totals->months[month_slot], 0, sizeof(totals->months[month_slot]));
            totals->months[month_slot].month_index = (int32_t)month_index;
        }
        totals->months[month_slot].mw_minutes += record->average_milliwatts;
        ++totals->months[month_slot].minute_count;
    }

    if (record->interval_start_utc > totals->folded_through_utc) {
        totals->folded_through_utc = record->interval_start_utc;
    }
    return true;
}

/* mW * minutes -> Wh: 1 Wh = 1000 mW * 60 min = 60000 mW*min. */
static double mw_minutes_to_wh(int64_t mw_minutes)
{
    return (double)mw_minutes / 60000.0;
}

double energy_totals_day_wh(const energy_totals_t *totals, int64_t utc_seconds)
{
    if (totals == NULL) {
        return 0.0;
    }
    int64_t day_start = energy_totals_day_start_utc(utc_seconds);
    for (size_t i = 0; i < ENERGY_TOTALS_DAY_SLOTS; ++i) {
        if (totals->days[i].day_utc == day_start) {
            return mw_minutes_to_wh(totals->days[i].mw_minutes);
        }
    }
    return 0.0;
}

double energy_totals_month_wh(const energy_totals_t *totals, int64_t utc_seconds)
{
    if (totals == NULL) {
        return 0.0;
    }
    int32_t month_index = energy_totals_month_index(utc_seconds);
    for (size_t i = 0; i < ENERGY_TOTALS_MONTH_SLOTS; ++i) {
        if (totals->months[i].month_index == month_index) {
            return mw_minutes_to_wh(totals->months[i].mw_minutes);
        }
    }
    return 0.0;
}

void energy_totals_make_blob(const energy_totals_t *totals, energy_totals_blob_t *blob)
{
    if (blob == NULL) {
        return;
    }
    memset(blob, 0, sizeof(*blob));
    blob->magic = ENERGY_TOTALS_MAGIC;
    blob->version = ENERGY_TOTALS_VERSION;
    if (totals != NULL) {
        blob->folded_through_utc = totals->folded_through_utc;
        memcpy(blob->days, totals->days, sizeof(blob->days));
        memcpy(blob->months, totals->months, sizeof(blob->months));
    }
    blob->checksum = crc32((const uint8_t *)blob, offsetof(energy_totals_blob_t, checksum));
}

bool energy_totals_blob_is_valid(const energy_totals_blob_t *blob)
{
    if (blob == NULL) {
        return false;
    }
    if (blob->magic != ENERGY_TOTALS_MAGIC || blob->version != ENERGY_TOTALS_VERSION) {
        return false;
    }
    return blob->checksum ==
           crc32((const uint8_t *)blob, offsetof(energy_totals_blob_t, checksum));
}

bool energy_totals_from_blob(const energy_totals_blob_t *blob, energy_totals_t *totals)
{
    if (totals == NULL) {
        return false;
    }
    energy_totals_reset(totals);
    if (!energy_totals_blob_is_valid(blob)) {
        return false;
    }
    totals->folded_through_utc = blob->folded_through_utc;
    memcpy(totals->days, blob->days, sizeof(totals->days));
    memcpy(totals->months, blob->months, sizeof(totals->months));
    return true;
}
