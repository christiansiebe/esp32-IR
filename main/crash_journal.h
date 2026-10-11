#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Persistent crash journal for the sender.
 *
 * The sender has no network, so the on-demand diagnostic ring only lives in
 * RAM and is lost on a crash. This journal is a small, checksummed snapshot
 * (counters + the tail of the diagnostic event ring) stored in NVS that
 * survives a reset. After a crash the receiver fetches it and the boot log
 * reports the reset reason of the previous session.
 *
 * The record is transferred as a single ESP-NOW payload (250 byte limit), so
 * the event tail is deliberately short.
 *
 * The pure serialization/validation helpers carry no ESP-IDF dependency so
 * they can be exercised by the host tests.
 */

#define CRASH_JOURNAL_MAGIC   0x4E524A43u /* "CJRN" */
#define CRASH_JOURNAL_VERSION 1u
#define CRASH_JOURNAL_EVENT_COUNT    3u
#define CRASH_JOURNAL_EVENT_TEXT_LEN 40u

typedef struct __attribute__((packed)) {
    uint64_t timestamp_ms;
    uint8_t level;
    char text[CRASH_JOURNAL_EVENT_TEXT_LEN];
} crash_journal_event_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t boot_count;
    /* How the session that wrote this record had started (esp_reset_reason_t). */
    int32_t reset_reason;
    /* Uptime reached when the record was last written. */
    uint64_t session_uptime_ms;
    uint32_t free_heap;
    uint32_t uart_rx_bytes;
    uint32_t fifo_overflows;
    uint32_t rx_buffer_full;
    uint32_t frame_errors;
    uint32_t parity_errors;
    uint32_t tx_queue_drops;
    uint32_t sml_sent_ok;
    uint32_t sml_send_fail;
    uint32_t diag_requests;
    uint32_t last_sequence;
    uint8_t event_count;
    /* 1 when the session ended through a registered shutdown handler (clean
     * reboot), 0 while running or after a crash. */
    uint8_t clean_shutdown;
    uint16_t reserved2;
    crash_journal_event_t events[CRASH_JOURNAL_EVENT_COUNT];
    uint32_t checksum;
} crash_journal_t;

/* Initialize a fresh record for a new session. */
void crash_journal_make_fresh(crash_journal_t *journal, uint32_t boot_count,
                              int32_t reset_reason);

/* Copy up to CRASH_JOURNAL_EVENT_COUNT events (chronological order; when more
 * are given the newest ones win) and finalize the checksum. */
void crash_journal_set_events(crash_journal_t *journal,
                              const crash_journal_event_t *events, size_t count);

/* Recompute the checksum over all preceding bytes. */
void crash_journal_finalize(crash_journal_t *journal);

uint32_t crash_journal_checksum(const crash_journal_t *journal);
bool crash_journal_is_valid(const crash_journal_t *journal);

/* Human-readable name for an esp_reset_reason_t value. */
const char *crash_journal_reset_reason_name(int reason);

/* NVS-backed persistence (implemented in crash_journal_store.c). */
esp_err_t crash_journal_save(const crash_journal_t *journal);
/* Loads the stored record; *found is set to false when none/valid record
 * exists. Returns ESP_OK in that case, not an error. */
esp_err_t crash_journal_load(crash_journal_t *journal, bool *found);
