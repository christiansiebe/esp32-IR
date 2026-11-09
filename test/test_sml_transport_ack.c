/* Host tests for the receiver's application-level ACK (sml_transport_ack_t).
 *
 * The build/parse pair is header-only so the sender and the receiver agree on
 * the wire format; a mismatch here would silently break delivery confirmation
 * (the sender would retransmit forever or stop retransmitting wrongly). */
#include <stdio.h>
#include <string.h>

#include "sml_transport.h"

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

static void test_round_trip(void)
{
    sml_transport_ack_t ack;
    sml_transport_ack_build(&ack, 0x12345678u);
    CHECK(ack.magic == SML_TRANSPORT_ACK_MAGIC, "magic set");
    CHECK(ack.version == SML_TRANSPORT_ACK_VERSION, "version set");
    CHECK(ack.sequence == 0x12345678u, "sequence echoed");

    uint32_t sequence = 0;
    CHECK(sml_transport_ack_parse(&ack, sizeof(ack), &sequence),
          "well-formed ACK parses");
    CHECK(sequence == 0x12345678u, "parsed sequence matches");

    /* A NULL out pointer is allowed. */
    CHECK(sml_transport_ack_parse(&ack, sizeof(ack), NULL),
          "parse accepts a NULL out pointer");
}

static void test_rejections(void)
{
    sml_transport_ack_t ack;
    sml_transport_ack_build(&ack, 7u);

    CHECK(!sml_transport_ack_parse(NULL, sizeof(ack), NULL),
          "NULL data rejected");
    CHECK(!sml_transport_ack_parse(&ack, sizeof(ack) - 1, NULL),
          "short frame rejected");
    CHECK(!sml_transport_ack_parse(&ack, sizeof(ack) + 1, NULL),
          "long frame rejected");

    /* A wrong magic is not an ACK (e.g. an SML telegram frame must not be
     * mistaken for one). */
    sml_transport_ack_t wrong = ack;
    wrong.magic = SML_TRANSPORT_MAGIC;
    CHECK(!sml_transport_ack_parse(&wrong, sizeof(wrong), NULL),
          "telegram magic rejected");

    wrong = ack;
    wrong.version = (uint8_t)(SML_TRANSPORT_ACK_VERSION + 1u);
    CHECK(!sml_transport_ack_parse(&wrong, sizeof(wrong), NULL),
          "unknown version rejected");
}

int main(void)
{
    test_round_trip();
    test_rejections();

    printf("sml transport ack: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
