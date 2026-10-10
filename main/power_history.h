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
void power_history_get_status(power_history_status_t *status);
