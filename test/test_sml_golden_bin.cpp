/* Golden decode test driven by real meter telegrams committed as .bin files
 * under test/fixtures/sml/.
 *
 * Each fixture is fed byte by byte through the production adapter
 * (sml_adapter_feed_byte), i.e. exactly the path the sender runs on raw UART
 * bytes, and the decoded OBIS values are locked to their golden numbers. The
 * adapter only ever hands values to the transport on SML_FINAL, so this also
 * proves the receiver's crc_valid flag: a valid telegram yields CRC_VALID with
 * its values, and a single flipped payload byte yields CRC_INVALID with none
 * (an "ABER-CRC" telegram must stay invalid, not silently pass).
 *
 * The fixture directory is argv[1] (the runner passes an absolute path); it
 * defaults to the in-repo location for manual runs.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* One decoded value observed in the adapter callback. */
typedef struct {
    uint8_t obis[6];
    long long value;
    int8_t scaler;
    uint8_t unit;
} decoded_t;

typedef struct {
    int crc_valid_calls;
    int crc_invalid_calls;
    int count;
    decoded_t values[SML_TRANSPORT_MAX_VALUES];
} capture_t;

static capture_t g_cap;

extern "C" void sml_adapter_telegram_callback(uint64_t, bool crc_valid,
                                              const sml_transport_value_t *values,
                                              uint8_t value_count)
{
    if (crc_valid) {
        ++g_cap.crc_valid_calls;
    } else {
        ++g_cap.crc_invalid_calls;
    }
    g_cap.count = value_count;
    for (uint8_t i = 0; i < value_count && i < SML_TRANSPORT_MAX_VALUES; ++i) {
        memcpy(g_cap.values[i].obis, values[i].obis, 6);
        g_cap.values[i].value = values[i].value;
        g_cap.values[i].scaler = values[i].scaler;
        g_cap.values[i].unit = values[i].unit;
    }
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
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

/* Feed a fixture through the adapter; returns the capture (in g_cap). */
static void feed_fixture(const char *dir, const char *name)
{
    memset(&g_cap, 0, sizeof(g_cap));
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
    sml_adapter_reset();
    for (size_t i = 0; i < len; ++i) {
        (void)sml_adapter_feed_byte(tg[i], (uint64_t)i * 1000u);
    }
    free(tg);
}

static const decoded_t *find_obis(const uint8_t *obis)
{
    for (int i = 0; i < g_cap.count; ++i) {
        if (memcmp(g_cap.values[i].obis, obis, 6) == 0) {
            return &g_cap.values[i];
        }
    }
    return NULL;
}

static void expect_value(const uint8_t *obis, long long value, int scaler,
                         uint8_t unit, const char *what)
{
    const decoded_t *v = find_obis(obis);
    if (v == NULL) {
        printf("  FAIL: %s missing\n", what);
        ++g_failures;
        ++g_checks;
        return;
    }
    CHECK(v->value == value, what);
    CHECK(v->scaler == scaler, what);
    CHECK(v->unit == unit, what);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "test/fixtures/sml";

    static const uint8_t OBIS_1_8_0[6] = {0x01, 0x00, 0x01, 0x08, 0x00, 0xff};
    static const uint8_t OBIS_2_8_0[6] = {0x01, 0x00, 0x02, 0x08, 0x00, 0xff};
    static const uint8_t OBIS_16_7_0[6] = {0x01, 0x00, 0x10, 0x07, 0x00, 0xff};

    /* EMH eHZ (optical): import energy only, 83916488 Wh. */
    feed_fixture(dir, "emh_ehz_hw8e2a5l0ek2p.bin");
    CHECK(g_cap.crc_valid_calls == 1 && g_cap.crc_invalid_calls == 0,
          "EMH eHZ telegram is CRC-valid exactly once");
    expect_value(OBIS_1_8_0, 83916488LL, -1, SML_WATT_HOUR, "EMH 1-0:1.8.0");

    /* EasyMeter Q3A: import + export energy and active power. */
    feed_fixture(dir, "easymeter_q3a.bin");
    CHECK(g_cap.crc_valid_calls == 1 && g_cap.crc_invalid_calls == 0,
          "EasyMeter telegram is CRC-valid exactly once");
    expect_value(OBIS_1_8_0, 29416461614LL, -4, SML_WATT_HOUR,
                 "EasyMeter 1-0:1.8.0");
    expect_value(OBIS_2_8_0, 1100731603LL, -4, SML_WATT_HOUR,
                 "EasyMeter 1-0:2.8.0");
    expect_value(OBIS_16_7_0, 81026LL, -2, SML_WATT, "EasyMeter 1-0:16.7.0");

    /* Holley DTZ541 - the meter the TODO claimed needs a Kermit CRC. It is a
     * standard X-25 telegram and decodes here with the unchanged parser. */
    feed_fixture(dir, "holley_dtz541.bin");
    CHECK(g_cap.crc_valid_calls == 1 && g_cap.crc_invalid_calls == 0,
          "Holley DTZ541 telegram is CRC-valid (standard X-25)");
    expect_value(OBIS_2_8_0, 3149260LL, -1, SML_WATT_HOUR, "Holley 1-0:2.8.0");
    expect_value(OBIS_16_7_0, 460LL, 0, SML_WATT, "Holley 1-0:16.7.0");

    /* ISKRA MT691: import energy + active power. */
    feed_fixture(dir, "iskra_mt691.bin");
    CHECK(g_cap.crc_valid_calls == 1 && g_cap.crc_invalid_calls == 0,
          "ISKRA telegram is CRC-valid exactly once");
    expect_value(OBIS_1_8_0, 1989273LL, -1, SML_WATT_HOUR, "ISKRA 1-0:1.8.0");
    expect_value(OBIS_16_7_0, 26LL, 0, SML_WATT, "ISKRA 1-0:16.7.0");

    /* Corrupt copy (one flipped payload byte, checksum untouched): the adapter
     * must report CRC_INVALID and hand no values to the transport. This is the
     * production guarantee behind the receiver's crc_valid flag. */
    feed_fixture(dir, "emh_ehz_hw8e2a5l0ek2p_crc_error.bin");
    CHECK(g_cap.crc_invalid_calls == 1 && g_cap.crc_valid_calls == 0,
          "flipped-byte copy is CRC-invalid exactly once");
    CHECK(g_cap.count == 0, "CRC-invalid telegram transports no values");

    printf("sml golden bin decode: %d checks, %d failures\n", g_checks,
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
