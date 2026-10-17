/* Host test for the AP-off decision (main/ap_off_policy.h).
 *
 * The rule guards the sender's channel discovery: the receiver may drop its
 * SoftAP only when the station's channel equals the ESP-NOW channel the sender
 * falls back to. The debounce keeps a mode switch from flapping the AP. */
#include <stdio.h>

#include "ap_off_policy.h"

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

int main(void)
{
    /* AP off only when the station sits on the ESP-NOW fallback channel. */
    CHECK(ap_off_allowed_on_channel(1, 1), "channel 1 == 1 -> allowed");
    CHECK(ap_off_allowed_on_channel(6, 6), "channel 6 == 6 -> allowed");
    CHECK(ap_off_allowed_on_channel(11, 11), "channel 11 == 11 -> allowed");
    CHECK(!ap_off_allowed_on_channel(6, 1), "channel 6 != 1 -> AP kept (late sender)");
    CHECK(!ap_off_allowed_on_channel(1, 6), "channel 1 != 6 -> AP kept");
    CHECK(!ap_off_allowed_on_channel(0, 0), "channel 0 is not a real channel -> AP kept");
    CHECK(!ap_off_allowed_on_channel(0, 1), "channel 0 vs 1 -> AP kept");

    /* Debounce: a disconnect right after a mode switch is not real. */
    CHECK(!ap_off_disconnect_is_real(0), "0 us after switch -> ignore");
    CHECK(!ap_off_disconnect_is_real(1 * 1000 * 1000), "1 s after switch -> ignore");
    CHECK(!ap_off_disconnect_is_real(4 * 1000 * 1000), "4 s after switch -> ignore");
    CHECK(!ap_off_disconnect_is_real(5 * 1000 * 1000), "exactly 5 s -> ignore (strict >)");
    CHECK(ap_off_disconnect_is_real(5 * 1000 * 1000 + 1), "just past 5 s -> real");
    CHECK(ap_off_disconnect_is_real(30 * 1000 * 1000), "30 s after switch -> real");

    printf("ap off policy: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
