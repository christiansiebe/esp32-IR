/* Host test: the USB Serial/JTAG-console receiver may light-sleep once the IDF
 * connection monitor (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION) protects the port.
 * That config makes the IDF hold a ESP_PM_NO_LIGHT_SLEEP lock while a USB host
 * is connected and drop it when the cable unplugs, so "sleep when unplugged,
 * console/flash while plugged in" is safe. Crucially the *knob* must be
 * accepted again (so `set sleep on` works), while still parking defaults and
 * apply() at light_sleep=0 unless the operator opts in (burst-lock needs the
 * CPU awake for precise window timing).
 *
 * Compiled with -DCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1 and
 * -DCONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=1 so it exercises the allowed branch.
 * Pure C, no IDF.
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
    /* The guard hangs on the connection monitor: with it on, light sleep is
     * no longer refused on a USB-console build. */
    CHECK(runtime_config_light_sleep_allowed(),
          "light sleep is allowed when USJ connection monitor protects the port");

    /* Defaults must still come up off: burst-lock needs the CPU awake for exact
     * window timing and CPU light sleep adds HTTP latency. */
    runtime_config_t cfg = runtime_config_defaults();
    CHECK(!cfg.light_sleep, "light sleep still defaults to off");

    /* The operator may now opt in (this was refused before). */
    char err[96];
    CHECK(runtime_config_set(&cfg, "sleep", "on", 160, err, sizeof(err)) &&
              cfg.light_sleep,
          "set sleep=on is accepted again");
    CHECK(strstr(err, "unavailable") == NULL,
          "the refusal that blocked opt-in is gone");
    CHECK(runtime_config_set(&cfg, "sleep", "off", 160, err, sizeof(err)) &&
              !cfg.light_sleep,
          "set sleep=off still works");

    /* A persisted light_sleep=1 blob survives clamp (it must NOT be forced off
     * here, because the operator may have legitimately enabled it). */
    runtime_config_t stored = runtime_config_defaults();
    stored.light_sleep = true;
    runtime_config_clamp(&stored, 160);
    CHECK(stored.light_sleep, "clamp preserves a persisted light_sleep=1");

    printf("runtime config (USB console + connection monitor): %d checks, "
           "%d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}