#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define POWER_HISTORY_CAPACITY 30000u
#define POWER_HISTORY_PAGE_SIZE 128u

typedef struct __attribute__((packed)) {
    uint32_t sequence;
    int64_t interval_start_utc;
    int32_t average_milliwatts;
    uint32_t sample_count;
    uint16_t format_version;
    uint16_t reserved;
    uint32_t checksum;
} power_history_record_t;

typedef struct {
    bool ready;
    uint32_t oldest_sequence;
    uint32_t latest_sequence;
    uint32_t record_count;
    uint32_t queue_drops;
    uint32_t write_errors;
} power_history_status_t;

esp_err_t power_history_init(void);
bool power_history_enqueue(const power_history_record_t *record);
esp_err_t power_history_read(uint32_t after_sequence, uint32_t recent_count,
                             power_history_record_t *records, size_t capacity,
                             size_t *record_count, power_history_status_t *status,
                             bool *overflow);
/* Single-Pass-Aggregation: iteriert alle gespeicherten Saetze genau einmal und
 * mittelt sie in feste Zeit-Buckets. Vermeidet O(n^2)-Paging bei vollem
 * Speicher (30000 Saetze). count != 0 markiert belegte Buckets. */
esp_err_t power_history_aggregate(int64_t window_start_utc, int64_t bucket_seconds,
                                  double *sum_w, uint32_t *count, int64_t *bucket_start_ms,
                                  size_t bucket_count, power_history_status_t *status);

/* Reine, hardwareunabhaengige Aggregations-Helfer (ohne NVS/SPIFFS/FreeRTOS),
 * damit dieselbe Logik auf dem Host getestet werden kann. */
void power_history_aggregate_init(int64_t window_start_utc, int64_t bucket_seconds,
                                  double *sum_w, uint32_t *count,
                                  int64_t *bucket_start_ms, size_t bucket_count);
esp_err_t power_history_aggregate_core(int64_t window_start_utc,
                                       int64_t bucket_seconds,
                                       const power_history_record_t *records,
                                       size_t record_count, double *sum_w,
                                       uint32_t *count, size_t bucket_count);
void power_history_get_status(power_history_status_t *status);
