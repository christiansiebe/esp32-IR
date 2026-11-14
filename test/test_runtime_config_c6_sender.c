/* Host test: the ESP32-C6 *sender* must default light sleep OFF.
 *
 * The sender enables adaptive light sleep by default (CONFIG_ESPNOW_SENDER_LIGHT_SLEEP
 * =y) to save power between SML telegram bursts. On the ESP32-C6 the primary
 * console is the USB Serial/JTAG peripheral, which light sleep powers down: the
 * /dev/ttyACM* port would never re-enumerate and the device is unreachable (the
 * regression that bricked the C6 receiver). The sender build must therefore keep
 * DFS but refuse the park.
 *
 * Compiled with -DCONFIG_IDF_TARGET_ESP32C6=1 -DCONFIG_ESPNOW_ROLE_SENDER=1
 * -DCONFIG_ESPNOW_SENDER_LIGHT_SLEEP=1 -DCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1 so
 * runtime_config.c takes the C6 + sender + USB-console combination. Pure C, no
 * IDF.
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
    /* The predicate the whole guard hangs on: even though the sender feature is
     * compiled in, a USB Serial/JTAG console forbids the park. */
    CHECK(!runtime_config_light_sleep_allowed(),
          "C6 sender refuses light sleep when the USB Serial/JTAG console is primary");

    runtime_config_t cfg = runtime_config_defaults();
    CHECK(!cfg.light_sleep,
          "C6 sender defaults light sleep off (the feature is on, the target forbids it)");

    /* The CPU ceiling still tracks the sender default (80 MHz), not the C6 top
     * clock: the role default wins over the target default. */
    CHECK(cfg.cpu_mhz == 80u, "C6 sender keeps the 80 MHz sender ceiling");

    /* Turning it on is refused with an explanation. */
    char err[96];
    runtime_config_t before = cfg;
    CHECK(!runtime_config_set(&cfg, "sleep", "on", 160, err, sizeof(err)),
          "set sleep=on is refused on the C6 sender");
    CHECK(!cfg.light_sleep && cfg.cpu_mhz == before.cpu_mhz,
          "the refused set leaves the config unchanged");
    CHECK(strstr(err, "unavailable") != NULL,
          "the refusal explains why (unavailable)");

    /* A blob written by an earlier build (light_sleep=1) is forced off. */
    runtime_config_t stored = runtime_config_defaults();
    stored.light_sleep = true;
    runtime_config_clamp(&stored, 160);
    CHECK(!stored.light_sleep,
          "clamp forces a persisted light_sleep=1 off on the C6 sender");

    /* render() tells the operator the knob is inert. */
    char out[256];
    runtime_config_render(&stored, out, sizeof(out));
    CHECK(strstr(out, "unavailable") != NULL,
          "render explains light sleep is unavailable on this build");

    printf("runtime config (C6 sender): %d checks, %d failures\n", g_checks,
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
