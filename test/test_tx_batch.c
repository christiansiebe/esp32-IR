/* Host tests for the sender-side ESP-NOW batching ring (main/tx_batch.c).
 *
 * The ring is pure C so batching, oldest-eviction and the sequence/backfill
 * arithmetic can be checked without a radio or a running sender. */
#include <stdio.h>
#include <string.h>

#include "tx_batch.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

static sml_tx_item_t make_item(int marker)
{
    sml_tx_item_t item = {0};
    item.timestamp_us = (uint64_t)marker * 1000000u;
    item.crc_valid = (marker % 2) == 0;
    item.value_count = 1;
    item.values[0].value = marker;
    return item;
}

static void test_fifo_and_sequence(void)
{
    sml_tx_batch_t b;
    sml_tx_batch_init(&b);
    CHECK(sml_tx_batch_count(&b) == 0, "starts empty");

    for (int i = 1; i <= 5; ++i) {
        uint32_t seq = 0;
        sml_tx_item_t item = make_item(i);
        CHECK(sml_tx_batch_push(&b, &item, &seq), "push ok");
        CHECK(seq == (uint32_t)i, "sequence starts at 1 and increments");
    }
    CHECK(sml_tx_batch_count(&b) == 5, "five buffered");

    /* FIFO: take returns oldest first, sequence preserved. */
    sml_tx_item_t out;
    uint32_t seq = 0;
    CHECK(sml_tx_batch_take(&b, &out, &seq), "take ok");
    CHECK(seq == 1 && out.values[0].value == 1, "oldest first");
    CHECK(out.crc_valid == false, "item payload preserved");
    CHECK(sml_tx_batch_count(&b) == 4, "count drops");

    while (sml_tx_batch_take(&b, &out, &seq)) {
    }
    CHECK(sml_tx_batch_count(&b) == 0, "drained");
    CHECK(!sml_tx_batch_take(&b, &out, &seq), "take on empty fails");
}

static void test_eviction_keeps_newest(void)
{
    sml_tx_batch_t b;
    sml_tx_batch_init(&b);

    for (uint32_t i = 1; i <= SML_TX_BATCH_CAPACITY + 3; ++i) {
        uint32_t seq = 0;
        sml_tx_item_t item = make_item((int)(i % 100));
        sml_tx_batch_push(&b, &item, &seq);
    }
    CHECK(sml_tx_batch_count(&b) == SML_TX_BATCH_CAPACITY, "ring stays full");
    CHECK(sml_tx_batch_dropped(&b) == 3, "three oldest evicted");

    /* The newest item (sequence CAPACITY+3) must still be present at the head
     * offset that follows the evicted ones, and readable in order. */
    sml_tx_item_t out;
    uint32_t seq = 0;
    CHECK(sml_tx_batch_take(&b, &out, &seq), "first take ok");
    CHECK(seq == 4, "oldest surviving sequence is 4");
}

static void test_capacity_survives_a_cycle(void)
{
    /* A 5 s cycle at ~1 telegram/s is ~5 telegrams; the ring must hold far more
     * so a single cycle never evicts. */
    sml_tx_batch_t b;
    sml_tx_batch_init(&b);
    for (int i = 0; i < 5; ++i) {
        uint32_t seq = 0;
        sml_tx_item_t item = make_item(i + 1);
        sml_tx_batch_push(&b, &item, &seq);
    }
    sml_tx_item_t out;
    uint32_t seq = 0;
    for (int i = 0; i < 5; ++i) {
        CHECK(sml_tx_batch_take(&b, &out, &seq), "cycle item taken");
    }
    CHECK(sml_tx_batch_dropped(&b) == 0, "no loss within a cycle");
}

static void test_missing_detection(void)
{
    sml_tx_batch_t b;
    sml_tx_batch_init(&b);
    CHECK(sml_tx_batch_missing(&b) == 0, "nothing assigned yet");

    uint32_t seq = 0;
    for (uint32_t i = 1; i <= 10; ++i) {
        sml_tx_item_t item = make_item((int)i);
        sml_tx_batch_push(&b, &item, &seq);
    }
    CHECK(sml_tx_batch_missing(&b) == 10, "all ten still unsent");

    sml_tx_batch_mark_sent(&b, 7);
    CHECK(sml_tx_batch_missing(&b) == 3, "sent through 7 leaves 8,9,10");

    sml_tx_batch_mark_sent(&b, 5); /* out of order / stale: must not regress */
    CHECK(sml_tx_batch_missing(&b) == 3, "mark_sent never regresses");
    sml_tx_batch_mark_sent(&b, 10);
    CHECK(sml_tx_batch_missing(&b) == 0, "fully sent");

    /* Eviction before sending is exactly what 'missing' must surface. */
    sml_tx_batch_t c;
    sml_tx_batch_init(&c);
    for (uint32_t i = 1; i <= SML_TX_BATCH_CAPACITY + 2; ++i) {
        sml_tx_item_t item = make_item((int)i);
        sml_tx_batch_push(&c, &item, &seq);
    }
    CHECK(sml_tx_batch_dropped(&c) == 2, "two dropped");
    CHECK(sml_tx_batch_missing(&c) == SML_TX_BATCH_CAPACITY + 2,
          "unsent count includes evicted ones");
    sml_tx_batch_mark_sent(&c, SML_TX_BATCH_CAPACITY + 1);
    CHECK(sml_tx_batch_missing(&c) == 1, "sequence 2 (evicted) still missing");
}

static void test_requeue_front_retries_oldest(void)
{
    /* A taken-but-undelivered entry must come back at the head so the next
     * flush retries it first, preserving the sequence order. */
    sml_tx_batch_t b;
    sml_tx_batch_init(&b);
    uint32_t seq = 0;
    for (uint32_t i = 1; i <= 3; ++i) {
        sml_tx_item_t item = make_item((int)i);
        sml_tx_batch_push(&b, &item, &seq);
    }
    sml_tx_item_t out;
    uint32_t taken_seq = 0;
    CHECK(sml_tx_batch_take(&b, &out, &taken_seq), "take first");
    CHECK(taken_seq == 1, "took sequence 1");
    CHECK(sml_tx_batch_requeue_front(&b, &out, taken_seq), "requeue ok");
    CHECK(sml_tx_batch_count(&b) == 3, "count restored");
    CHECK(sml_tx_batch_dropped(&b) == 0, "requeue into space does not drop");

    sml_tx_item_t again;
    uint32_t again_seq = 0;
    CHECK(sml_tx_batch_take(&b, &again, &again_seq), "take again");
    CHECK(again_seq == 1, "the retried entry is oldest again");
    CHECK(again.values[0].value == 1, "payload preserved across requeue");
    /* The rest are still in order. */
    CHECK(sml_tx_batch_take(&b, &again, &again_seq) && again_seq == 2, "then 2");
    CHECK(sml_tx_batch_take(&b, &again, &again_seq) && again_seq == 3, "then 3");
    CHECK(sml_tx_batch_count(&b) == 0, "drained");
}

static void test_requeue_full_ring_evicts_oldest(void)
{
    /* Requeueing into a full ring must evict the oldest queued entry (counted),
     * keeping the ring invariant while the retried entry stays at the head. */
    sml_tx_batch_t b;
    sml_tx_batch_init(&b);
    uint32_t seq = 0;
    for (uint32_t i = 1; i <= SML_TX_BATCH_CAPACITY; ++i) {
        sml_tx_item_t item = make_item((int)(i % 100));
        sml_tx_batch_push(&b, &item, &seq);
    }
    sml_tx_item_t out;
    uint32_t taken_seq = 0;
    CHECK(sml_tx_batch_take(&b, &out, &taken_seq), "take from full ring");
    /* Refill so the ring is full again before the requeue. */
    sml_tx_item_t extra = make_item(7);
    CHECK(sml_tx_batch_push(&b, &extra, &seq), "refill");
    CHECK(sml_tx_batch_count(&b) == SML_TX_BATCH_CAPACITY, "full again");
    CHECK(sml_tx_batch_requeue_front(&b, &out, taken_seq), "requeue into full");
    CHECK(sml_tx_batch_count(&b) == SML_TX_BATCH_CAPACITY, "still full");
    CHECK(sml_tx_batch_dropped(&b) == 1, "one oldest entry evicted");
    sml_tx_item_t first;
    uint32_t first_seq = 0;
    CHECK(sml_tx_batch_take(&b, &first, &first_seq), "take head");
    CHECK(first_seq == taken_seq, "the retried entry is at the head");
}

int main(void)
{
    test_fifo_and_sequence();
    test_eviction_keeps_newest();
    test_capacity_survives_a_cycle();
    test_missing_detection();
    test_requeue_front_retries_oldest();
    test_requeue_full_ring_evicts_oldest();
    printf("test_tx_batch: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
