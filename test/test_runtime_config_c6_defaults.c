/* Host test: the ESP32-C6 receiver must default to a *loss-free* power-saving
 * combo.
 *
 * Bench 2026-11-08 on .105 (C6 receiver, classic sender, ~60 telegrams/min)
 * showed the station PS modes park the modem so hard that WIFI_PS_MIN/MAX_MODEM
 * *without* the ESP-NOW wake window drop ~90% of the telegrams (measured 90.8%,
 * 97.5%, 96.0% in three runs; transport_losses stays ~0 - they are genuine
 * radio gaps). With the wake window on and a short 20/15 ms window the loss
 * vanishes (0.8%, the ambient baseline) while the modem still sleeps 15 of
 * every 20 ms.
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

    /* The station PS mode must NOT be the default: min/max without the wake
     * window is the ~90%-loss combination. */
    CHECK(cfg.wifi_ps_mode == RUNTIME_CONFIG_PS_OFF,
          "C6 receiver defaults to station PS off (min/max lose ~90% of frames)");

    /* The ESP-NOW wake window is the lever that makes the sleep loss-free, and
     * it must be on with a short window. */
    CHECK(cfg.radio_save, "C6 receiver defaults to radio-save on");
    CHECK(cfg.wake_interval_ms == 20u,
          "C6 receiver defaults to a 20 ms wake interval (the measured sweet spot)");
    CHECK(cfg.wake_window_ms == 15u,
          "C6 receiver defaults to a 15 ms wake window");
    CHECK(cfg.wake_window_ms <= cfg.wake_interval_ms,
          "the wake window fits inside the interval (the radio really sleeps)");

    /* The CPU ceiling tracks the C6 top clock. */
    CHECK(cfg.cpu_mhz == 160u, "C6 receiver defaults to the 160 MHz ceiling");

    /* A round-trip through render() keeps the new defaults visible. */
    char out[512];
    runtime_config_render(&cfg, out, sizeof(out));
    CHECK(strstr(out, "wifi ps: off") != NULL, "render reports wifi ps off");
    CHECK(strstr(out, "radio save: on (20 ms / 15 ms window)") != NULL,
          "render reports the 20/15 wake window");

    printf("runtime config (C6 receiver defaults): %d checks, %d failures\n",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
