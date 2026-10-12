#include "tx_batch.h"

#include <string.h>

void sml_tx_batch_init(sml_tx_batch_t *b)
{
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->next_sequence = 1;
}

bool sml_tx_batch_push(sml_tx_batch_t *b, const sml_tx_item_t *item,
                       uint32_t *out_sequence)
{
    if (b == NULL || item == NULL) {
        return false;
    }
    if (b->count == SML_TX_BATCH_CAPACITY) {
        /* Drop the oldest so the newest is always kept. */
        b->head = (uint16_t)((b->head + 1) % SML_TX_BATCH_CAPACITY);
        --b->count;
        ++b->dropped;
    }
    sml_tx_batch_entry_t *slot = &b->entries[b->tail];
    slot->sequence = b->next_sequence++;
    slot->item = *item;
    b->tail = (uint16_t)((b->tail + 1) % SML_TX_BATCH_CAPACITY);
    ++b->count;
    if (out_sequence != NULL) {
        *out_sequence = slot->sequence;
    }
    return true;
}

bool sml_tx_batch_take(sml_tx_batch_t *b, sml_tx_item_t *out,
                       uint32_t *out_sequence)
{
    if (b == NULL || out == NULL || b->count == 0) {
        return false;
    }
    sml_tx_batch_entry_t *slot = &b->entries[b->head];
    *out = slot->item;
    if (out_sequence != NULL) {
        *out_sequence = slot->sequence;
    }
    b->head = (uint16_t)((b->head + 1) % SML_TX_BATCH_CAPACITY);
    --b->count;
    return true;
}

uint16_t sml_tx_batch_count(const sml_tx_batch_t *b)
{
    return b == NULL ? 0 : b->count;
}

uint32_t sml_tx_batch_dropped(const sml_tx_batch_t *b)
{
    return b == NULL ? 0 : b->dropped;
}

void sml_tx_batch_mark_sent(sml_tx_batch_t *b, uint32_t sequence)
{
    if (b != NULL && sequence > b->sent_sequence) {
        b->sent_sequence = sequence;
    }
}

uint32_t sml_tx_batch_missing(const sml_tx_batch_t *b)
{
    if (b == NULL) {
        return 0;
    }
    uint32_t assigned = b->next_sequence - 1; /* highest sequence handed out */
    if (b->sent_sequence >= assigned) {
        return 0;
    }
    return assigned - b->sent_sequence;
}
