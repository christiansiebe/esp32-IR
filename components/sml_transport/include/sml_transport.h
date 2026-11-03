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

/* Sender sleep-state announcement.
 *
 * The sender transmits this before the CPU parks for a light-sleep window and
 * again when it wakes, so the receiver always knows whether the sender is
 * reachable *right now* without having to poll for it. Packed into the same
 * ESP-NOW broadcast channel as the SML telegrams.
 *
 *   state  1 = about to sleep, 0 = awake (sent right after a real sleep)
 *   window_ms  the adaptive light-sleep window the sender is about to use
 *   sequence   monotonically increasing; lets the receiver tell duplicates
 */
#define SML_TRANSPORT_SLEEP_MAGIC 0x534C5031u /* "SLP1" */
#define SML_TRANSPORT_SLEEP_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t state;        /* 1 = sleeping (about to), 0 = awake */
    uint32_t sequence;    /* monotonic */
    uint32_t window_ms;   /* adaptive sleep window */
    uint32_t uptime_ms;   /* sender uptime for correlation */
} sml_transport_sleep_state_t;
