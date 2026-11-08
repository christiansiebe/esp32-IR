/* Host test: on a USB Serial/JTAG-console build WITH the
 * CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION guard compiled in (the shipped ESP32-C6
 * image), light sleep is allowed again -- the USJ driver holds
 * ESP_PM_NO_LIGHT_SLEEP for as long as a host is attached, so the console is
 * never lost. The C6 receiver therefore defaults light sleep ON, and the knob
 * is settable. This is the counterpart to test_runtime_config_usb_console.c
 * (same build but WITHOUT the guard, where light sleep must stay refused).
 *
 * Compiled with -DCONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1
 * -DCONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=1. Pure C, no IDF. */
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
    /* With the guard the predicate flips: light sleep is safe to offer. */
    CHECK(runtime_config_light_sleep_allowed(),
          "light sleep is allowed once the USB-host guard is compiled in");

    char err[96];
    runtime_config_t cfg = runtime_config_defaults();
    CHECK(cfg.light_sleep,
          "light sleep defaults to on for the C6 receiver behind the guard");

    /* The knob is settable in both directions and round-trips. */
    CHECK(runtime_config_set(&cfg, "sleep", "off", 160, err, sizeof(err)) &&
              !cfg.light_sleep,
          "set sleep=off");
    CHECK(runtime_config_set(&cfg, "sleep", "on", 160, err, sizeof(err)) &&
              cfg.light_sleep,
          "set sleep=on is accepted behind the guard");
    CHECK(runtime_config_set(&cfg, "light-sleep", "true", 160, err, sizeof(err)) &&
              cfg.light_sleep,
          "the light-sleep alias is accepted too");

    /* clamp() must NOT force a persisted light_sleep=1 off on this build. */
    runtime_config_t stored = runtime_config_defaults();
    stored.light_sleep = true;
    runtime_config_clamp(&stored, 160);
    CHECK(stored.light_sleep,
          "clamp keeps a persisted light_sleep=1 when the guard is present");

    /* render() shows the plain on/off, not the "unavailable" note. */
    char out[256];
    runtime_config_render(&stored, out, sizeof(out));
    CHECK(strstr(out, "light sleep: on") != NULL,
          "render shows light sleep on");
    CHECK(strstr(out, "unavailable") == NULL,
          "render no longer says light sleep is unavailable");

    printf("runtime config (USB guard): %d checks, %d failures\n", g_checks,
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
