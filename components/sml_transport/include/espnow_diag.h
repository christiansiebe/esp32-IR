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
/* 2: the counters section carries the sender firmware version (fw_version), so
 * the receiver can show which sender build is running. */
#define ESPNOW_DIAG_VERSION 2u
/* Oldest version still accepted. The header layout never changed and the
 * counters payload only grew at the tail, so a receiver on version 1 keeps
 * working against a version 2 sender (and vice versa). Without this, bumping
 * the version silently broke an un-reflashed peer: its requests were dropped. */
#define ESPNOW_DIAG_VERSION_MIN 1u

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
/* The receiver pulls missing minute records from the sender's own SPIFFS
 * history (backfill after a radio gap or a restart). The request carries the
 * UTC window and a paging cursor; the response is one page of records, with
 * ESPNOW_DIAG_FLAG_MORE set while further records follow. The wire structs are
 * declared next to the history store in main.c because they embed
 * power_history_record_t. */
#define ESPNOW_DIAG_SECTION_HISTORY  0x0010u
/* 7 records: header (16) + 12 byte trailer + 7*28 = 224 fits ESP_NOW_MAX_DATA_LEN
 * (250); 8 records would need 252 and not fit a single ESP-NOW payload. */
#define ESPNOW_DIAG_HISTORY_RECORDS_PER_PACKET 7u

/* Sender command/response channel (the receiver-hosted sender web console). The
 * request carries one command line; the sender runs it locally and answers with
 * the captured output. The wire structs live next to the console command table
 * in main.c. */
#define ESPNOW_DIAG_SECTION_CMD 0x0020u
#define ESPNOW_DIAG_CMD_LINE_LEN 96u
#define ESPNOW_DIAG_CMD_OUTPUT_LEN 192u

/* Sender firmware version (from version.c's fw_version(), e.g. "v0.1.0-3-gabc"
 * or "unknown"). Length includes the terminating NUL. */
#define ESPNOW_DIAG_FW_VERSION_LEN 24u

/* Response flag: at least one more event page follows for this request. */
#define ESPNOW_DIAG_FLAG_MORE 0x8000u
/* Request flag (OR'd with SECTION_HISTORY): full-store sync. The sender ignores
 * to_utc/after_sequence and treats the request's from_utc as a time cursor,
 * answering with time-ordered pages via power_history_read_after(), so the
 * receiver can mirror the sender's whole ring (including the oldest record that
 * a zero-window read cannot reach). The response carries oldest_utc/latest_utc. */
#define ESPNOW_DIAG_FLAG_SYNC_FULL 0x4000u

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
    /* Batching / reconciliation (sender's 5 s ESP-NOW send cycle). */
    uint32_t batch_pending;     /* telegrams currently held in the batch ring */
    uint32_t batch_missing;     /* assigned sequences never confirmed sent */
    /* Storage (SPIFFS power history) and power management (variable CPU clock /
     * sleep). `storage_ready` is 0 when the store fell back to RAM. */
    uint8_t storage_ready;      /* 1 = persistent SPIFFS history mounted */
    uint8_t sleep_state;        /* 0 = awake, 1 = light sleep, 2 = deep sleep */
    uint8_t cpu_freq_mhz;       /* current CPU clock */
    uint8_t cpu_freq_max_mhz;   /* configured DFS maximum */
    uint8_t cpu_freq_min_mhz;   /* configured DFS minimum (idle) */
    uint8_t light_sleep_enabled;/* DFS config allows light sleep in idle */
    uint8_t wifi_ps_mode;       /* Wi-Fi power save (modem sleep): 0=off,1=min,2=max */
    uint8_t reserved2;
    uint32_t storage_records;   /* minute records currently stored */
    uint32_t storage_capacity;  /* ring capacity */
    uint32_t storage_write_errors;
    uint32_t storage_queue_drops;
    uint32_t uptime_sleep_ms;   /* time spent in light sleep this session */
    /* Adaptive sender light sleep (CONFIG_ESPNOW_SENDER_LIGHT_SLEEP). */
    uint32_t sleep_window_ms;   /* current adaptive sleep window */
    uint32_t sleep_min_gap_ms;  /* shortest inter-telegram quiet observed */
    uint8_t sleep_hold_active;  /* 1 = awake due to a history pull / OTA */
    uint8_t reserved3[3];
    /* CPU clock (DFS) history, sampled once per second: the average/min/max MHz
     * the clock reached, and how often it stepped. `cpu_freq_mhz` above is the
     * live value; these show how hard the variable clock worked this session. */
    uint8_t cpu_freq_avg_mhz;
    uint8_t cpu_freq_hist_min_mhz;
    uint8_t cpu_freq_hist_max_mhz;
    uint8_t gap_last_cause;     /* why the most recent gap happened (see below) */
    uint32_t cpu_freq_changes;
    /* Receive-gap attribution: how many of the gaps were caused by the meter's
     * own pause, by a light-sleep window, by a long stall without sleep, or by a
     * full UART RX buffer. Lets the receiver answer "where does the gap come
     * from?" without guessing. */
    uint32_t gap_cause_meter;
    uint32_t gap_cause_sleep;
    uint32_t gap_cause_stall;
    uint32_t gap_cause_buffer;
    /* Boot marker: the sender fills the offline window of a restart/OTA/battery
     * with reason-tagged minute records, so the receiver can tell an expected
     * outage apart from a real data loss. Counts the minutes filled for the
     * previous session's gap, and why (POWER_HISTORY_REASON_*). */
    uint32_t boot_marker_count;
    uint8_t boot_marker_reason;
    /* Build identity: 0 = plain sender, 1 = CONFIG_ESPNOW_SENDER_LIGHT_SLEEP.
     * Lets the diagnostics page (and the receiver) tell which sender image is
     * actually running, so a flashed-but-inactive sleep path is unambiguous. */
    uint8_t build_profile;
    /* Sender firmware version string (fw_version()), NUL-terminated. Empty when
     * the build did not provide one (host build without build_version.h). */
    char fw_version[ESPNOW_DIAG_FW_VERSION_LEN];
    /* 1 when the link fallback fired on the sender (light sleep dropped, radio
     * kept awake); reuses the first reserved byte so the struct keeps its size
     * and no wire-version bump is needed. */
    uint8_t sleep_fallback_active;
    uint8_t reserved4;
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
