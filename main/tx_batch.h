#pragma once

/* Sender-side batching buffer for the UART-decoded SML telegrams.
 *
 * The sender no longer fires one ESP-NOW packet per telegram. It pushes each
 * decoded telegram into this ring and a periodic send phase drains it, so the
 * radio is idle between cycles. No IDF dependency, so the ring and the
 * sequence/backfill arithmetic are host-tested (test/test_tx_batch.c).
 *
 * Sequence contract: every pushed item gets a strictly increasing sequence,
 * assigned by the buffer. A full ring evicts the *oldest* item (and counts it),
 * which keeps the sequence of the newest item contiguous with what the sender
 * later reports - the receiver can therefore detect a gap from the sequence
 * alone (see sml_tx_batch_missing()).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sml_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Room for far more than one 5 s cycle (telegrams arrive ~1/s) before evicting. */
#define SML_TX_BATCH_CAPACITY 256u

/* One decoded telegram, before it is packed for ESP-NOW. */
typedef struct {
    uint64_t timestamp_us;
    bool crc_valid;
    uint8_t value_count;
    sml_transport_value_t values[SML_TRANSPORT_MAX_VALUES];
} sml_tx_item_t;

typedef struct {
    uint32_t sequence;
    sml_tx_item_t item;
} sml_tx_batch_entry_t;

typedef struct {
    sml_tx_batch_entry_t entries[SML_TX_BATCH_CAPACITY];
    uint16_t head;         /* next slot to read */
    uint16_t tail;         /* next slot to write */
    uint16_t count;        /* occupied slots */
    uint32_t dropped;      /* items evicted because the ring was full */
    uint32_t next_sequence; /* sequence to assign on the next push */
    uint32_t sent_sequence; /* highest sequence handed to ESP-NOW (0 = none) */
} sml_tx_batch_t;

void sml_tx_batch_init(sml_tx_batch_t *b);

/* Append a copy of item; assigns and returns its sequence. When the ring is
 * full the oldest entry is dropped (dropped counter increments) so the newest
 * data is always kept. Returns false only on a NULL argument. */
bool sml_tx_batch_push(sml_tx_batch_t *b, const sml_tx_item_t *item,
                       uint32_t *out_sequence);

/* Pop the oldest entry. Returns false when empty. */
bool sml_tx_batch_take(sml_tx_batch_t *b, sml_tx_item_t *out,
                       uint32_t *out_sequence);

uint16_t sml_tx_batch_count(const sml_tx_batch_t *b);
uint32_t sml_tx_batch_dropped(const sml_tx_batch_t *b);

/* Record that the given sequence was successfully handed to ESP-NOW. Sending is
 * in order, so this just advances the high-water mark. */
void sml_tx_batch_mark_sent(sml_tx_batch_t *b, uint32_t sequence);

/* Number of sequences in [1, highest assigned] that were never handed to
 * ESP-NOW (i.e. evicted before sending). 0 in normal operation; a growing value
 * is the reconciliation signal the receiver can act on. */
uint32_t sml_tx_batch_missing(const sml_tx_batch_t *b);

#ifdef __cplusplus
}
#endif
