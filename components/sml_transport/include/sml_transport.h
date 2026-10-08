#pragma once

#include <stdint.h>

#define SML_TRANSPORT_MAGIC 0x534D4C31u
#define SML_TRANSPORT_VERSION 1u
#define SML_TRANSPORT_MAX_VALUES 3u

typedef struct __attribute__((packed)) {
    uint8_t obis[6];
    int64_t value;
    int8_t scaler;
    uint8_t unit;
} sml_transport_value_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t crc_valid;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint8_t value_count;
} sml_transport_header_t;

typedef struct __attribute__((packed)) {
    sml_transport_header_t header;
    sml_transport_value_t values[SML_TRANSPORT_MAX_VALUES];
} sml_transport_packet_t;
