/* Built-in SML parser self-test for the firmware test console.
 *
 * Decodes the same golden eHZ telegram the host golden test uses, so `test sml`
 * on the device proves the parser and its OBIS tables decode real bytes
 * correctly - no meter and no bench setup needed. Lives in the C++ parser
 * component so both the sender and the receiver link it. */
#include "sml_selftest.h"

#include "sml.h"

#include "sml_golden_fixture.h"

extern "C" void sml_selftest_run(sml_selftest_result_t *result)
{
    if (result == nullptr) {
        return;
    }
    *result = sml_selftest_result_t{};

    static const unsigned char OBIS_1_8_0[6] = {0x01, 0x00, 0x01, 0x08, 0x00, 0xff};
    static const unsigned char OBIS_1_8_1[6] = {0x01, 0x00, 0x01, 0x08, 0x01, 0xff};
    static const unsigned char OBIS_15_7_0[6] = {0x01, 0x00, 0x0f, 0x07, 0x00, 0xff};

    smlReset();
    sml_states_t state = SML_START;
    long long v180 = -1;
    long long v181 = -1;
    long long v1570 = -1;
    signed char scaler = 0;
    int listends = 0;

    for (unsigned int i = 0; i < ehz_bin_len; ++i) {
        /* smlState() takes a mutable reference, so feed a copy (the fixture is
         * const and lives in read-only memory). */
        unsigned char byte = ehz_bin[i];
        state = smlState(byte);
        if (state == SML_LISTEND) {
            ++listends;
            if (smlOBISCheck(OBIS_1_8_0)) {
                smlOBISByUnitChecked(v180, scaler, SML_WATT_HOUR);
            } else if (smlOBISCheck(OBIS_1_8_1)) {
                smlOBISByUnitChecked(v181, scaler, SML_WATT_HOUR);
            } else if (smlOBISCheck(OBIS_15_7_0)) {
                smlOBISByUnitChecked(v1570, scaler, SML_WATT);
            }
        }
    }

    result->crc_valid = (state == SML_FINAL);
    result->energy_1_8_0 = v180;
    result->energy_1_8_1 = v181;
    result->power_15_7_0 = v1570;
    result->listends = listends;
    result->pass = result->crc_valid && v180 == 0x74ea && v181 == 0x12d687 &&
                   v1570 == 0x2f65;
}
