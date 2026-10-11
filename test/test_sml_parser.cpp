/* Host robustness tests for the vendored SML parser.
 *
 * The sender parses raw UART bytes straight off the meter, so a malformed or
 * truncated telegram must never read or write outside the parser's static
 * buffers. This test feeds adversarial byte streams through smlState() and is
 * built with AddressSanitizer, so any out-of-bounds access to nodes[],
 * listBuffer[] or the OBIS reads fails the run.
 */
#include <stdint.h>
#include <stdio.h>

#include "sml.h"
#include "sml_adapter.h"

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

static const unsigned char OBIS_IMPORT_ENERGY[6] = {0x01, 0x00, 0x01, 0x08,
                                                    0x00, 0xff};

/* A valid telegram starts with 1B 1B 1B 1B, the start byte 01, the version 01
 * and a 3-byte length (00 00 00); only then does the block/list parsing begin. */
static void feed_start(void)
{
    static const unsigned char start[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x01,
                                          0x01, 0x00, 0x00, 0x00};
    for (size_t i = 0; i < sizeof(start); ++i) {
        unsigned char byte = start[i];
        (void)smlState(byte);
    }
}

static void feed(const unsigned char *bytes, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        unsigned char byte = bytes[i];
        (void)smlState(byte);
    }
}

/* smlNewList() is the parser's list-nesting entry point (not declared in the
 * public header). Driving it directly makes the nodes[] depth guard
 * deterministic instead of depending on a crafted byte stream. */
void smlNewList(unsigned char size);

/* sml_adapter.cpp normally depends on the transport header; for the host test
 * we only need the "awaiting telegram" predicate, so stub the callback it
 * declares. */
extern "C" void sml_adapter_telegram_callback(uint64_t, bool,
                                              const sml_transport_value_t *,
                                              uint8_t)
{
}

bool sml_adapter_awaiting_telegram(void);

int main(void)
{
    /* OBIS accessors on an empty/short list must not read past the buffer. */
    smlReset();
    CHECK(!smlOBISCheck(OBIS_IMPORT_ENERGY),
          "smlOBISCheck on empty list returns false");
    long long value = 0;
    signed char scaler = 0;
    CHECK(!smlOBISByUnitChecked(value, scaler, SML_WATT_HOUR),
          "smlOBISByUnitChecked on empty list returns false");

    /* Deeply nested lists: each smlNewList() pushes a node. With the old
     * off-by-one guard (currentLevel < MAX_TREE_SIZE) this walks one element
     * past nodes[]; the fix keeps currentLevel in range. */
    smlReset();
    for (int i = 0; i < 64; ++i) {
        smlNewList(1);
    }
    CHECK(1, "deeply nested list did not crash");

    /* A truncated data list: header claims more bytes than are supplied, then
     * the stream stops. The OBIS helpers must bail out instead of walking past
     * listBuffer. */
    smlReset();
    feed_start();
    static const unsigned char truncated[] = {0x76, 0x01, 0x62, 0x01, 0x62};
    feed(truncated, sizeof(truncated));
    (void)smlOBISCheck(OBIS_IMPORT_ENERGY);
    (void)smlOBISByUnitChecked(value, scaler, SML_WATT_HOUR);
    CHECK(1, "truncated list did not crash");

    /* Pseudo-random garbage: fixed seed so the test is deterministic. A valid
     * telegram header is injected periodically so the stream also reaches the
     * deep block/list parsing states instead of only the preamble. */
    smlReset();
    uint32_t rng = 0x12345678u;
    for (int i = 0; i < 200000; ++i) {
        if (i % 1000 == 0) {
            smlReset();
            feed_start();
        }
        rng = rng * 1103515245u + 12345u;
        unsigned char byte = (unsigned char)(rng >> 16);
        sml_states_t state = smlState(byte);
        if (state == SML_LISTEND) {
            (void)smlOBISCheck(OBIS_IMPORT_ENERGY);
            (void)smlOBISByUnitChecked(value, scaler, SML_WATT_HOUR);
        }
    }
    CHECK(1, "random byte stream did not crash");

    /* The gap diagnosis relies on sml_adapter_awaiting_telegram(): only
     * SML_START and SML_FINAL are "between telegrams". A gap anywhere else
     * would mean bytes were lost inside a telegram. */
    smlReset();
    CHECK(sml_adapter_awaiting_telegram(),
          "idle parser (SML_START) counts as between telegrams");
    feed_start();
    static const unsigned char partial[] = {0x77, 0x07, 0x01};
    feed(partial, sizeof(partial));
    CHECK(!sml_adapter_awaiting_telegram(),
          "parser in the middle of a telegram is not 'awaiting'");
    smlReset();
    CHECK(sml_adapter_awaiting_telegram(),
          "parser is between telegrams again after a reset");

    printf("sml parser robustness: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
