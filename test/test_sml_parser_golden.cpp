/* Golden decode test for the vendored SML parser.
 *
 * A real eHZ telegram (EMH eHZ, from the public libsml-testing corpus, carried
 * by olliiiver/sml_parser's example fixture) is fed byte by byte through
 * smlState(). This locks the parser's byte-level output - states and decoded
 * OBIS values - so a future parser/table change that silently corrupts decoding
 * fails here instead of shipping a wrong meter reading.
 */
#include <stdint.h>
#include <stdio.h>

#include "sml.h"

#include "fixtures/sml/ehz_bin.upstream.h"

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

/* The eHZ fixture carries these OBIS lists (all in message 2): 1-0:1.8.0,
 * 1-0:1.8.1, 1-0:1.8.2 (energy, Wh) and 1-0:15.7.0 (power, W). There is no
 * 2.8.0 / 16.7.0 in this telegram. */
static const unsigned char OBIS_1_8_0[6] = {0x01, 0x00, 0x01, 0x08, 0x00, 0xff};
static const unsigned char OBIS_1_8_1[6] = {0x01, 0x00, 0x01, 0x08, 0x01, 0xff};
static const unsigned char OBIS_15_7_0[6] = {0x01, 0x00, 0x0f, 0x07, 0x00, 0xff};

int main(void)
{
    smlReset();
    sml_states_t state = SML_START;
    long long v180 = -1, v181 = -1, v1570 = -1;
    signed char scaler = 0;
    int listends = 0;

    for (unsigned int i = 0; i < ehz_bin_len; ++i) {
        unsigned char byte = ehz_bin[i];
        state = smlState(byte);
        if (state == SML_LISTEND) {
            ++listends;
            if (smlOBISCheck(OBIS_1_8_0)) {
                CHECK(smlOBISByUnitChecked(v180, scaler, SML_WATT_HOUR),
                      "1-0:1.8.0 decodes with the Wh unit");
            } else if (smlOBISCheck(OBIS_1_8_1)) {
                CHECK(smlOBISByUnitChecked(v181, scaler, SML_WATT_HOUR),
                      "1-0:1.8.1 decodes with the Wh unit");
            } else if (smlOBISCheck(OBIS_15_7_0)) {
                CHECK(smlOBISByUnitChecked(v1570, scaler, SML_WATT),
                      "1-0:15.7.0 decodes with the W unit");
            }
        }
    }

    CHECK(state == SML_FINAL, "telegram ends with a valid checksum (SML_FINAL)");
    printf("  info: listends=%d 1.8.0=%lld 1.8.1=%lld 15.7.0=%lld\n",
           listends, v180, v181, v1570);

    /* Golden values for the eHZ fixture (regression lock, not a spec claim):
     * 1-0:1.8.0 = 0x000074ea, 1-0:1.8.1 = 0x0012d687, 1-0:15.7.0 = 0x2f65. */
    CHECK(v180 == 0x74ea, "1-0:1.8.0 raw value matches the fixture");
    CHECK(v181 == 0x12d687, "1-0:1.8.1 raw value matches the fixture");
    CHECK(v1570 == 0x2f65, "1-0:15.7.0 raw value matches the fixture");

    /* A single flipped payload byte must be rejected: the parser reaches
     * SML_CHECKSUM_ERROR and never SML_FINAL. This is the guarantee that the
     * receiver's crc_valid flag rests on. */
    smlReset();
    state = SML_START;
    for (unsigned int i = 0; i < ehz_bin_len; ++i) {
        unsigned char byte = (i == 30) ? (unsigned char)(ehz_bin[i] ^ 0x01)
                                       : ehz_bin[i];
        state = smlState(byte);
    }
    CHECK(state == SML_CHECKSUM_ERROR,
          "a flipped payload byte yields SML_CHECKSUM_ERROR, not SML_FINAL");

    printf("sml golden decode: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
