/* Host test for the pure OTA safety policy (main/ota_policy.c).
 *
 * Two guards keep an OTA from destroying the measurement history or bricking
 * the device: the update slot must not overlap the SPIFFS "storage" partition,
 * and a pulled image URL must be https. Both are pure and tested here with the
 * real production addresses from partitions.csv. */
#include <stdio.h>
#include <string.h>

#include "ota_policy.h"

static int g_checks;
static int g_failures;
#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

/* Real layout (partitions.csv): ota_0 @0x20000/0x180000, ota_1 @0x1a0000/0x180000,
 * storage @0x320000/0xE0000. */
#define OTA0_START 0x020000u
#define OTA0_SIZE  0x180000u
#define OTA1_START 0x1A0000u
#define OTA1_SIZE  0x180000u
#define STOR_START 0x320000u
#define STOR_SIZE  0x0E0000u

int main(void)
{
    /* URL guard: only an https scheme is trusted. */
    CHECK(ota_url_is_trusted_policy("https://host/files/esp32-IR.bin"), "https accepted");
    CHECK(!ota_url_is_trusted_policy("http://host/files/esp32-IR.bin"), "plain http rejected");
    CHECK(!ota_url_is_trusted_policy("HTTP://host/x.bin"), "upper-case scheme rejected");
    CHECK(!ota_url_is_trusted_policy("https:/host/x.bin"), "short scheme rejected");
    CHECK(!ota_url_is_trusted_policy("ftp://host/x.bin"), "ftp rejected");
    CHECK(!ota_url_is_trusted_policy("host/x.bin"), "scheme-less rejected");
    CHECK(!ota_url_is_trusted_policy(""), "empty rejected");
    CHECK(!ota_url_is_trusted_policy(NULL), "NULL rejected");
    CHECK(ota_url_is_trusted_policy("https://"), "bare scheme accepted (client will fail later)");

    /* Range disjointness: half-open intervals, touching is disjoint. */
    CHECK(ota_ranges_disjoint(0, 100, 200, 100), "gap -> disjoint");
    CHECK(ota_ranges_disjoint(200, 100, 0, 100), "gap (reversed) -> disjoint");
    CHECK(ota_ranges_disjoint(0, 100, 100, 100), "touching edges -> disjoint");
    CHECK(!ota_ranges_disjoint(0, 100, 50, 100), "overlap -> not disjoint");
    CHECK(!ota_ranges_disjoint(50, 100, 0, 100), "overlap (reversed) -> not disjoint");
    CHECK(!ota_ranges_disjoint(0, 100, 0, 100), "identical -> not disjoint");
    CHECK(ota_ranges_disjoint(0, 0, 50, 100), "zero-size -> disjoint");
    CHECK(ota_ranges_disjoint(50, 100, 0, 0), "zero-size (other) -> disjoint");

    /* The real slots must be safe against the real storage partition. */
    CHECK(ota_target_is_safe_range(OTA0_START, OTA0_SIZE, STOR_START, STOR_SIZE),
          "ota_0 does not overlap storage");
    CHECK(ota_target_is_safe_range(OTA1_START, OTA1_SIZE, STOR_START, STOR_SIZE),
          "ota_1 does not overlap storage");

    /* A fat app slot that grows into storage must be refused. 0x20000+0x310000
     * ends at 0x330000, past the storage start 0x320000. (A size that ends
     * exactly at 0x320000 would only touch, which is allowed.) */
    CHECK(!ota_target_is_safe_range(OTA0_START, 0x310000u, STOR_START, STOR_SIZE),
          "oversized slot overlapping storage is refused");
    CHECK(!ota_target_is_safe_range(STOR_START, 0x1000, STOR_START, STOR_SIZE),
          "slot exactly on storage is refused");
    CHECK(!ota_target_is_safe_range(STOR_START - 0x1000, 0x2000, STOR_START, STOR_SIZE),
          "slot straddling the storage start is refused");
    CHECK(!ota_target_is_safe_range(STOR_START + STOR_SIZE - 0x1000, 0x2000,
                                    STOR_START, STOR_SIZE),
          "slot straddling the storage end is refused");

    /* No measurement partition configured: always safe. */
    CHECK(ota_target_is_safe_range(OTA0_START, 0x300000u, 0, 0),
          "absent storage -> always safe");

    printf("ota policy: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
