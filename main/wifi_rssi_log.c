#include "wifi_rssi_log.h"

#include <string.h>

void wifi_rssi_log_init(wifi_rssi_log_t *log, uint32_t period_ms, uint64_t now_ms)
{
    if (log == NULL) {
        return;
    }
    memset(log, 0, sizeof(*log));
    log->period_ms = period_ms;
    log->last_sample_ms = now_ms;
    log->have_last = false;
    log->latest_dbm = 0;
    log->min_dbm = 0;
    log->max_dbm = 0;
}

bool wifi_rssi_log_due(const wifi_rssi_log_t *log, uint64_t now_ms)
{
    if (log == NULL || log->period_ms == 0) {
        return false;
    }
    if (!log->have_last) {
        /* Never sampled: due once the period has passed since init. */
        return now_ms >= log->last_sample_ms &&
               (now_ms - log->last_sample_ms) >= log->period_ms;
    }
    if (now_ms < log->last_sample_ms) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    return (now_ms - log->last_sample_ms) >= log->period_ms;
}

void wifi_rssi_log_add(wifi_rssi_log_t *log, int8_t rssi_dbm, bool valid,
                       uint64_t now_ms)
{
    if (log == NULL) {
        return;
    }
    /* Advance the period even on a miss; clamp a backwards clock so the next
     * sample is not delayed by the whole skew. */
    log->last_sample_ms = now_ms;
    log->have_last = true;

    wifi_rssi_sample_t *sample = &log->samples[log->next];
    sample->uptime_ms = (uint32_t)now_ms;
    sample->valid = valid;
    sample->rssi_dbm = valid ? rssi_dbm : 0;
    log->next = (log->next + 1u) % WIFI_RSSI_LOG_CAPACITY;
    if (log->count < WIFI_RSSI_LOG_CAPACITY) {
        ++log->count;
    }

    if (!valid) {
        ++log->miss_count;
        return;
    }
    ++log->valid_count;
    log->sum_dbm += rssi_dbm;
    log->latest_dbm = rssi_dbm;
    if (log->valid_count == 1) {
        log->min_dbm = rssi_dbm;
        log->max_dbm = rssi_dbm;
    } else {
        if (rssi_dbm < log->min_dbm) {
            log->min_dbm = rssi_dbm;
        }
        if (rssi_dbm > log->max_dbm) {
            log->max_dbm = rssi_dbm;
        }
    }
}

int wifi_rssi_log_average_dbm(const wifi_rssi_log_t *log)
{
    if (log == NULL || log->valid_count == 0) {
        return 0;
    }
    /* Round to nearest; sum is a signed 32-bit accumulator. */
    int32_t avg = log->sum_dbm / (int32_t)log->valid_count;
    return (int)avg;
}
