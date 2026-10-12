#pragma once

#include <stdint.h>

/*
 * On-demand sender diagnostics over ESP-NOW.
 *
 * This protocol is intentionally separate from the SML transport
 * (SML_TRANSPORT_MAGIC) and from the OTA control protocol
 * (ESPNOW_OTA_MAGIC) so that neither path is altered. A request is sent
 * unicast by the receiver to the sender MAC learned from received SML
 * packets; the sender answers unicast so that the ESP-NOW send callback
 * reports a real delivery status (MAC level ACK). No diagnostics are sent
 * unless the receiver explicitly asks for them.
 */

#define ESPNOW_DIAG_MAGIC   0x44494147u /* "DIAG" */
#define ESPNOW_DIAG_VERSION 1u

#define ESPNOW_DIAG_TYPE_REQUEST  1u
#define ESPNOW_DIAG_TYPE_RESPONSE 2u

/* Requested/returned sections (request flags and response flags). */
#define ESPNOW_DIAG_SECTION_COUNTERS 0x0001u
#define ESPNOW_DIAG_SECTION_EVENTS   0x0002u
#define ESPNOW_DIAG_SECTION_CRASH    0x0004u
/* The sender asks the receiver for the wall clock (the receiver is the only
 * side with an NTP-synced clock). The request carries the sender's uptime; the
 * response carries the receiver's UTC seconds. */
#define ESPNOW_DIAG_SECTION_TIME_SYNC 0x0008u

/* Response flag: at least one more event page follows for this request. */
#define ESPNOW_DIAG_FLAG_MORE 0x8000u

#define ESPNOW_DIAG_EVENT_TEXT_LEN    40u
#define ESPNOW_DIAG_EVENT_RING        16u
#define ESPNOW_DIAG_EVENTS_PER_PACKET 4u

/* Event severities stored in espnow_diag_event_t.level. */
#define ESPNOW_DIAG_LEVEL_INFO  0u
#define ESPNOW_DIAG_LEVEL_WARN  1u
#define ESPNOW_DIAG_LEVEL_ERROR 2u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t request_id;
    uint32_t page;
} espnow_diag_header_t;

typedef struct __attribute__((packed)) {
    uint64_t uptime_ms;
    uint64_t uart_rx_bytes;
    uint32_t fifo_overflows;
    uint32_t rx_buffer_full;
    uint32_t frame_errors;
    uint32_t parity_errors;
    uint32_t tx_queue_drops;
    uint32_t sml_sent_ok;
    uint32_t sml_send_fail;
    uint32_t diag_requests;
    uint32_t last_sequence;
    uint32_t free_heap;
    int8_t last_send_status;
    int8_t last_request_rssi;
    uint8_t channel;
    uint8_t reserved;
    /* Transport-gap diagnosis (see the sender's uart_capture_worker). */
    uint32_t gap_count;
    uint32_t gap_quiet;         /* gap while waiting for a telegram */
    uint32_t gap_mid;           /* gap inside a telegram */
    uint32_t last_gap_ms;       /* length of the most recent gap */
    uint32_t max_gap_ms;        /* longest gap in this session */
    uint32_t last_rx_ago_ms;    /* time since the last received byte */
    uint32_t gap_loss;          /* mid-telegram gaps that failed the checksum */
    uint32_t parser_resyncs;    /* parser resets after a truncated telegram */
    uint32_t time_syncs;        /* successful wall-clock calibrations */
    uint32_t time_requests;     /* wall-clock requests sent to the receiver */
} espnow_diag_counters_t;

typedef struct __attribute__((packed)) {
    uint64_t timestamp_ms;
    uint8_t level;
    char text[ESPNOW_DIAG_EVENT_TEXT_LEN];
} espnow_diag_event_t;

typedef struct __attribute__((packed)) {
    espnow_diag_header_t header;
    uint8_t event_total;
    uint8_t event_count;
    espnow_diag_event_t events[ESPNOW_DIAG_EVENTS_PER_PACKET];
} espnow_diag_events_response_t;

/* Time-sync exchange. The sender sends its uptime; the receiver answers with
 * its NTP-synced UTC seconds. The sender keeps offset = utc - uptime/1e6 and
 * stamps events with real time until the next calibration. */
typedef struct __attribute__((packed)) {
    espnow_diag_header_t header;
    uint64_t uptime_ms;         /* request: sender uptime at send */
    int64_t utc_seconds;        /* response: receiver UTC (0 = not synced) */
    uint8_t valid;              /* response: 1 when utc_seconds is trustworthy */
} espnow_diag_time_sync_t;
