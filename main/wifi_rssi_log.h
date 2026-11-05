#pragma once

/* Sender-only: periodic signal-strength log of the home WLAN.
 *
 * The sender is deliberately NOT associated with the home WLAN - it only uses
 * ESP-NOW on the channel the receiver's SoftAP shares with the home AP. It still
 * helps to know how well that AP is heard, so the sender scans for the home
 * SSID once per period and records the RSSI. It never associates, so this cannot
 * disturb the ESP-NOW link (see the glue in main.c: the scan is limited to the
 * current ESP-NOW channel, so the radio does not even leave it).
 *
 * This header holds only the pure bookkeeping - the due decision, the sample
 * ring and the session statistics - so it is host-tested
 * (test/test_wifi_rssi_log.c) without any ESP-IDF.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One hour of minutes at the default period; enough to eyeball a trend without
 * a flash write (the ring is RAM-only and rolls). */
#define WIFI_RSSI_LOG_CAPACITY 60u

typedef struct {
    uint32_t uptime_ms; /* monotonic stamp of the sample */
    int8_t rssi_dbm;    /* 0 when no AP matched (see `valid`) */
    bool valid;         /* false when the scan did not find the SSID */
} wifi_rssi_sample_t;

typedef struct {
    uint32_t period_ms;      /* how often to sample */
    uint64_t last_sample_ms; /* stamp of the last sample (or init time) */
    bool have_last;          /* a sample has been taken since init */
    wifi_rssi_sample_t samples[WIFI_RSSI_LOG_CAPACITY];
    size_t count;            /* valid entries in `samples` (<= capacity) */
    size_t next;             /* write cursor into `samples` */
    /* Session statistics over the valid samples only. */
    int8_t latest_dbm;
    int8_t min_dbm;
    int8_t max_dbm;
    int32_t sum_dbm;
    uint32_t valid_count;
    uint32_t miss_count; /* scans that did not find the SSID */
} wifi_rssi_log_t;

/* Initialise at boot. The first sample is due `period_ms` after `now_ms`, so a
 * slow boot does not sample immediately. */
void wifi_rssi_log_init(wifi_rssi_log_t *log, uint32_t period_ms, uint64_t now_ms);

/* True when `period_ms` has elapsed since the last sample. Read-only: the
 * caller scans, then calls wifi_rssi_log_add(). */
bool wifi_rssi_log_due(const wifi_rssi_log_t *log, uint64_t now_ms);

/* Record one scan result. `rssi_dbm` is ignored when `valid` is false; a miss
 * still advances the period so a hidden/absent AP does not cause a busy loop.
 * A backwards clock (now < last_sample) is clamped so the period is not
 * stretched. */
void wifi_rssi_log_add(wifi_rssi_log_t *log, int8_t rssi_dbm, bool valid,
                       uint64_t now_ms);

/* Average RSSI over the valid samples this session, or 0 when none. */
int wifi_rssi_log_average_dbm(const wifi_rssi_log_t *log);
