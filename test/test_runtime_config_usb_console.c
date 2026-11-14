/* Host test: light sleep must be refused on a build whose primary console is
 * the USB Serial/JTAG peripheral (the ESP32-C6, which has no USB-UART bridge).
 *
 * Compiled with -DCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1 so runtime_config.c takes
 * the "console on USB" branch. This is the regression that bricked the C6: the
 * console accepted `set sleep on`, persisted light_sleep=1 in NVS, and the next
 * boot never re-enumerated /dev/ttyACM* because light sleep powers the PHY down.
 * The knob must now be refused, a persisted value forced off, and apply() must
 * never hand light_sleep_enable=1 to esp_pm_configure(). Pure C, no IDF.
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
    /* The predicate the whole guard hangs on. */
    CHECK(!runtime_config_light_sleep_allowed(),
          "light sleep is refused when the USB Serial/JTAG console is primary");

    char err[96];
    runtime_config_t cfg = runtime_config_defaults();
    CHECK(!cfg.light_sleep, "light sleep defaults to off on a USB-console build");

    /* Turning it on is refused: this is exactly the value that used to be
     * persisted and killed the USB port. */
    runtime_config_t before = cfg;
    CHECK(!runtime_config_set(&cfg, "sleep", "on", 160, err, sizeof(err)),
          "set sleep=on is refused");
    CHECK(!cfg.light_sleep && cfg.cpu_mhz == before.cpu_mhz,
          "the refused set leaves the config unchanged");
    CHECK(strstr(err, "unavailable") != NULL,
          "the refusal explains why (unavailable)");
    CHECK(!runtime_config_set(&cfg, "light-sleep", "1", 160, err, sizeof(err)),
          "the light-sleep=1 alias is refused too");
    CHECK(!runtime_config_set(&cfg, "light-sleep", "true", 160, err, sizeof(err)),
          "the light-sleep=true alias is refused too");

    /* Turning it off is still accepted (idempotent, and what a recovery run
     * wants to be able to say). */
    CHECK(runtime_config_set(&cfg, "sleep", "off", 160, err, sizeof(err)) &&
              !cfg.light_sleep,
          "set sleep=off is still accepted");

    /* A blob written by an earlier build (light_sleep=1) is forced off by
     * clamp(), which runtime_config_load() applies to every restored value. */
    runtime_config_t stored = runtime_config_defaults();
    stored.light_sleep = true;
    runtime_config_clamp(&stored, 160);
    CHECK(!stored.light_sleep,
          "clamp forces a persisted light_sleep=1 off");

    /* apply() must never pass light_sleep_enable=1 to esp_pm_configure() on this
     * build, even when a stale struct still says so (host apply() is a no-op,
     * but it must not depend on that). */
    runtime_config_t stale = runtime_config_defaults();
    stale.light_sleep = true;
    CHECK(runtime_config_apply(&stale),
          "apply() still succeeds (the host stub returns true)");

    /* render() tells the operator why the knob is inert. */
    char out[256];
    runtime_config_render(&stored, out, sizeof(out));
    CHECK(strstr(out, "unavailable") != NULL,
          "render explains light sleep is unavailable on this build");

    printf("runtime config (USB console): %d checks, %d failures\n", g_checks,
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
