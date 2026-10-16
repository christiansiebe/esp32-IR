/* Host test for the built-in SML self-test used by the console's `test sml`.
 *
 * The device command must report PASS on a healthy parser, so this locks the
 * self-test's own verdict (and its decoded values) against the golden fixture.
 */
#include <cstdio>

#include "sml_selftest.h"

int main(void)
{
    sml_selftest_result_t r;
    sml_selftest_run(&r);

    int failures = 0;
    if (!r.pass) {
        ++failures;
        printf("  FAIL: self-test did not pass\n");
    }
    if (!r.crc_valid) {
        ++failures;
        printf("  FAIL: telegram did not end with a valid checksum\n");
    }
    if (r.energy_1_8_0 != 0x74ea || r.energy_1_8_1 != 0x12d687 ||
        r.power_15_7_0 != 0x2f65) {
        ++failures;
        printf("  FAIL: decoded values differ from the fixture\n");
    }

    printf("sml self-test: %d checks, %d failures\n", 4, failures);
    return failures == 0 ? 0 : 1;
}
