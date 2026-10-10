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
