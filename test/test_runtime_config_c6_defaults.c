/* Host test: the ESP32-C6 receiver must default to a *loss-free* AND low-power
 * combo.
 *
 * Bench 2026-11-08 on .105 (C6 receiver, classic sender, ~60 telegrams/min)
 * showed the station PS modes park the modem so hard that WIFI_PS_MIN/MAX_MODEM
 * *without* the ESP-NOW wake window drop ~90% of the telegrams (measured 90.8%,
 * 97.5%, 96.0% in three runs; transport_losses stays ~0 - they are genuine
 * radio gaps). The driver's own wake window fixes that but drifts against the
 * sender's burst, so a window short enough to save power drops bursts.
 *
 * The burst lock removes the drift: it learns the burst period and holds the
 * modem awake only around the predicted burst, so the station can actually
 * sleep (ps min) in between. Measured on .105 (300 s windows, interleaved):
 * ps min + burst lock + radio-save off tracks the always-awake baseline within
 * the ambient noise (3.3-5.4% vs 4.7-6.0%; the residual is 2.4 GHz interference,
 * present even with the radio never parked), while the modem is awake only ~15%
 * of the time (vs the 75% duty of the 20/15 wake window).
 *
 * Compiled with -DCONFIG_IDF_TARGET_ESP32C6=1 so runtime_config.c takes the C6
 * branch; the classic ESP32 default is asserted unchanged by
 * test_runtime_config.c. Pure C, no IDF.
 */
#include <stdio.h>
#include <string.h>

#include "runtime_config.h"

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

int main(void)
{
    runtime_config_t cfg = runtime_config_defaults();

    /* The burst lock is the lever that makes modem sleep loss-free: it opens a
     * short RX window around the predicted burst instead of a free-running one.
     * It must be on, and it needs a station PS mode to park the modem. */
    CHECK(cfg.burst_lock, "C6 receiver defaults to burst-lock on");
    CHECK(cfg.wifi_ps_mode == RUNTIME_CONFIG_PS_MIN,
          "C6 receiver defaults to station PS min (the burst lock covers the burst)");

    /* The free-running wake window must be OFF: the burst lock drives the radio
     * directly, and the two fight for it if both are on. */
    CHECK(!cfg.radio_save, "C6 receiver defaults to radio-save off (burst lock drives the radio)");
    CHECK(cfg.wake_window_ms == 150u,
          "the burst window is 150 ms (covers the burst with phase-jitter margin)");

    /* The CPU ceiling tracks the C6 top clock. */
    CHECK(cfg.cpu_mhz == 160u, "C6 receiver defaults to the 160 MHz ceiling");

    /* A round-trip through render() keeps the new defaults visible. */
    char out[512];
    runtime_config_render(&cfg, out, sizeof(out));
    CHECK(strstr(out, "wifi ps: min") != NULL, "render reports wifi ps min");
    CHECK(strstr(out, "burst lock: on") != NULL, "render reports the burst lock on");
    CHECK(strstr(out, "radio save: off") != NULL, "render reports radio-save off");

    printf("runtime config (C6 receiver defaults): %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
