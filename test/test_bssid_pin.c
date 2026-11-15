/* Sticky BSSID pin with fail-open: take a pin on the first association, hold it
 * across reconnects, release it once the pinned BSSID has been gone for
 * failopen_ms so the station can never wedge itself offline. Pure logic, no
 * ESP-IDF. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bssid_pin.h"

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

#define FAILOPEN 60000u

static const uint8_t MAC_A[BSSID_PIN_MAC_LEN] = {0xdc, 0x15, 0xc8, 0xdf, 0x84, 0x44};
static const uint8_t MAC_B[BSSID_PIN_MAC_LEN] = {0xdc, 0x15, 0xc8, 0xdf, 0x84, 0x45};

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, BSSID_PIN_MAC_LEN) == 0;
}

static void test_take_on_first_connect(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);
    uint8_t got[BSSID_PIN_MAC_LEN];

    CHECK(!bssid_pin_get(&p, got), "no pin before any connect");

    CHECK(bssid_pin_note_connected(&p, MAC_A, true, 1000), "takes a pin on connect");
    CHECK(p.valid, "pin is valid");
    CHECK(p.pins == 1, "one pin taken");
    CHECK(bssid_pin_get(&p, got) && mac_eq(got, MAC_A), "pin holds MAC_A");

    /* A second association with the pin held must not take a new one. */
    CHECK(!bssid_pin_note_connected(&p, MAC_B, true, 2000),
          "held pin is not replaced on reconnect");
    CHECK(p.pins == 1, "pin count unchanged");
    CHECK(bssid_pin_get(&p, got) && mac_eq(got, MAC_A), "pin still MAC_A");
}

static void test_no_pin_when_disallowed(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);

    CHECK(!bssid_pin_note_connected(&p, MAC_A, false, 1000),
          "does not take a pin when not allowed");
    CHECK(!p.valid, "still unpinned");
}

static void test_failopen_release(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);
    uint8_t got[BSSID_PIN_MAC_LEN];

    (void)bssid_pin_note_connected(&p, MAC_A, true, 1000);

    CHECK(!bssid_pin_note_disconnect(&p, false, 3000), "no release before fail-open");
    CHECK(p.valid, "pin survives a short gap");

    CHECK(bssid_pin_note_disconnect(&p, false, 61000), "releases at fail-open");
    CHECK(!p.valid, "pin released");
    CHECK(p.releases == 1, "one release counted");
    CHECK(!bssid_pin_get(&p, got), "no pin after release");

    /* After a release the next connect may pick any BSSID and pin again. */
    CHECK(bssid_pin_note_connected(&p, MAC_B, true, 62000),
          "takes a new pin after a release");
    CHECK(p.pins == 2, "second pin counted");
    CHECK(bssid_pin_get(&p, got) && mac_eq(got, MAC_B), "new pin holds MAC_B");
}

static void test_connect_rearms_failopen(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);

    bssid_pin_set(&p, MAC_A, 0);
    CHECK(!bssid_pin_note_connected(&p, MAC_A, true, 50000),
          "held pin: reconnect only re-arms");

    /* 40 s after the last connect: still inside the window. */
    CHECK(!bssid_pin_note_disconnect(&p, false, 90000), "no release 40 s after connect");
    /* 65 s after the last connect: the AP is gone. */
    CHECK(bssid_pin_note_disconnect(&p, false, 115000), "release 65 s after last connect");
}

static void test_explicit_set_and_clear(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);
    uint8_t got[BSSID_PIN_MAC_LEN];

    bssid_pin_set(&p, MAC_A, 0);
    CHECK(p.valid && p.pins == 1, "manual set pins");
    bssid_pin_set(&p, MAC_B, 0);
    CHECK(p.pins == 2, "manual set overwrites and counts");
    CHECK(bssid_pin_get(&p, got) && mac_eq(got, MAC_B), "manual set holds MAC_B");

    bssid_pin_clear(&p);
    CHECK(!p.valid, "clear drops the pin");
    CHECK(!bssid_pin_get(&p, got), "no pin after clear");
}

static void test_restore_arms_at_now(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);
    uint8_t got[BSSID_PIN_MAC_LEN];

    /* A pin restored at boot must be armed at the restore time, not at 0: a
     * disconnect right after boot must not release it. */
    bssid_pin_restore(&p, MAC_A, 70000);
    CHECK(p.valid, "restored pin is valid");
    CHECK(p.pins == 0, "restore does not count as a new pin");
    CHECK(bssid_pin_get(&p, got) && mac_eq(got, MAC_A), "restored pin holds MAC_A");
    CHECK(!bssid_pin_note_disconnect(&p, false, 71000),
          "no release 1 s after a restore");
    CHECK(bssid_pin_note_disconnect(&p, false, 135000),
          "release 65 s after a restore");
}

static void test_unreachable_releases_at_once(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);
    uint8_t got[BSSID_PIN_MAC_LEN];

    bssid_pin_set(&p, MAC_A, 0);
    /* The pinned AP is gone (NO_AP_FOUND): release immediately, do not wait the
     * window - the receiver must not stay offline to keep a pin. */
    CHECK(bssid_pin_note_disconnect(&p, true, 1000),
          "unreachable BSSID releases at once");
    CHECK(!p.valid, "pin released immediately");
    CHECK(p.releases == 1, "one release counted");
    CHECK(!bssid_pin_get(&p, got), "no pin after immediate release");
}

static void test_clock_rollback_safe(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);

    bssid_pin_set(&p, MAC_A, 100000);
    /* A disconnect seen with an earlier clock must not release early. */
    CHECK(!bssid_pin_note_disconnect(&p, false, 50000), "rollback does not release");
    CHECK(p.valid, "pin survives a clock rollback");
}

static void test_null_safe(void)
{
    bssid_pin_t p;
    bssid_pin_init(&p, FAILOPEN);

    bssid_pin_set(&p, NULL, 0);
    CHECK(!p.valid, "NULL bssid set is ignored");
    CHECK(!bssid_pin_note_connected(&p, NULL, true, 0), "NULL bssid connect ignored");
    CHECK(!bssid_pin_get(&p, NULL), "NULL out is refused");
}

int main(void)
{
    test_take_on_first_connect();
    test_no_pin_when_disallowed();
    test_failopen_release();
    test_connect_rearms_failopen();
    test_explicit_set_and_clear();
    test_restore_arms_at_now();
    test_unreachable_releases_at_once();
    test_clock_rollback_safe();
    test_null_safe();

    printf("test_bssid_pin: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
