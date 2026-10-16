#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of the built-in SML parser self-test (the same fixture and OBIS values
 * the host golden test locks). `crc_valid` is the parser's final state. */
typedef struct {
    bool pass;
    bool crc_valid;
    long long energy_1_8_0;
    long long energy_1_8_1;
    long long power_15_7_0;
    int listends;
} sml_selftest_result_t;

/* Decode the golden eHZ telegram byte by byte and report the decoded values, so
 * the test console can prove the parser is healthy without any hardware. */
void sml_selftest_run(sml_selftest_result_t *result);

#ifdef __cplusplus
}
#endif
