/* Host test for the SML CRC-16 variants (components/sml_parser/src/sml_crc16.c).
 *
 * Motivation: the vendored parser checks one CRC parametrisation, but other SML
 * projects (spacemanspiff2007/sml2mqtt via SmlLib) try two - X-25 *and* Kermit -
 * because "not every meter uses the standard CRC". This test pins the two
 * algorithms against their published check values, proves they share the one
 * reflected table, and then checks real meter telegrams from the public
 * devZer0/libsml-testing corpus: every one of them validates as X-25 (including
 * the Holley DTZ541, which the TODO claimed would need Kermit), so the parser's
 * existing choice is correct and needs no blind change.
 *
 * The fixture directory is passed as argv[1] (the runner passes an absolute
 * path); it defaults to the in-repo location for manual runs.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sml_crc16.h"

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

/* The stored checksum sits in the last two telegram bytes as low, high; the
 * parser reads it little-endian (`low | (high << 8)`), which is what
 * sml_crc16_x25() returns. */
static uint16_t wire_checksum(const uint8_t *tg, size_t len)
{
    return (uint16_t)(tg[len - 2] | ((uint16_t)tg[len - 1] << 8));
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)n);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

/* Cross-check a real telegram: the on-wire checksum must equal X-25 over the
 * body (all bytes except the trailing checksum). */
static void check_fixture(const char *dir, const char *name, int expect_x25)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    size_t len = 0;
    uint8_t *tg = read_file(path, &len);
    if (tg == NULL) {
        printf("  FAIL: cannot read fixture %s\n", path);
        ++g_failures;
        ++g_checks;
        return;
    }
    uint16_t stored = wire_checksum(tg, len);
    uint16_t x25 = sml_crc16_x25(tg, len - 2);
    uint16_t kermit = sml_crc16_kermit(tg, len - 2);
    if (expect_x25) {
        CHECK(x25 == stored, name);
        CHECK(kermit != stored, name);
    } else {
        CHECK(x25 != stored, name);
    }
    free(tg);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "test/fixtures/sml";

    /* Published check values: CRC-16/X-25 -> 0x906E, CRC-16/KERMIT -> 0x2189
     * for the ASCII string "123456789" (reveng CRC catalogue). */
    static const uint8_t chk[] = "123456789";
    CHECK(sml_crc16_x25(chk, sizeof(chk) - 1) == 0x906E,
          "X-25 check value is 0x906E");
    CHECK(sml_crc16_kermit(chk, sizeof(chk) - 1) == 0x2189,
          "Kermit check value is 0x2189");

    /* The two variants differ only in init / final XOR over the same reflected
     * table: X-25 = raw(0xFFFF) ^ 0xFFFF, Kermit = raw(0x0000). */
    uint16_t raw = 0xFFFF;
    for (size_t i = 0; i < sizeof(chk) - 1; ++i) {
        raw = sml_crc16_update(chk[i], raw);
    }
    CHECK((raw ^ 0xFFFF) == sml_crc16_x25(chk, sizeof(chk) - 1),
          "X-25 is the reflected table with init 0xFFFF + final XOR");
    raw = 0x0000;
    for (size_t i = 0; i < sizeof(chk) - 1; ++i) {
        raw = sml_crc16_update(chk[i], raw);
    }
    CHECK(raw == sml_crc16_kermit(chk, sizeof(chk) - 1),
          "Kermit is the reflected table with init 0x0000, no final XOR");

    /* The dispatcher agrees with the named wrappers. */
    CHECK(sml_crc16(chk, sizeof(chk) - 1, SML_CRC16_X25) ==
              sml_crc16_x25(chk, sizeof(chk) - 1),
          "dispatcher X-25 matches the wrapper");
    CHECK(sml_crc16(chk, sizeof(chk) - 1, SML_CRC16_KERMIT) ==
              sml_crc16_kermit(chk, sizeof(chk) - 1),
          "dispatcher Kermit matches the wrapper");

    /* Empty input: X-25 -> 0x0000 (0xFFFF ^ 0xFFFF), Kermit -> 0x0000. */
    CHECK(sml_crc16_x25(NULL, 0) == 0x0000, "X-25 of empty input is 0x0000");
    CHECK(sml_crc16_kermit(NULL, 0) == 0x0000,
          "Kermit of empty input is 0x0000");

    /* Real telegrams: all committed fixtures validate as X-25. The corrupt
     * copy (one flipped payload byte, checksum untouched) must not. */
    check_fixture(dir, "emh_ehz_hw8e2a5l0ek2p.bin", 1);
    check_fixture(dir, "easymeter_q3a.bin", 1);
    check_fixture(dir, "holley_dtz541.bin", 1);
    check_fixture(dir, "iskra_mt691.bin", 1);
    check_fixture(dir, "emh_ehz_hw8e2a5l0ek2p_crc_error.bin", 0);

    printf("sml crc16 variants: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
