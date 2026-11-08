/* Host test for the status-LED decision logic (main/led_status.c).
 *
 * The colour/blink mapping drives the receiver's WS2812 status LED, so a wrong
 * phase or colour is a silent, hard-to-spot regression. Pure C, no IDF. */
#include <stdio.h>
#include <string.h>

#include "led_status.h"

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

static bool color_is(led_status_t s, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t cr = 0, cg = 0, cb = 0;
    led_status_color(s, &cr, &cg, &cb);
    return cr == r && cg == g && cb == b;
}

int main(void)
{
    /* Base colours. */
    CHECK(color_is(LED_STATUS_BOOT, 255, 255, 255), "boot is white");
    CHECK(color_is(LED_STATUS_WIFI_CONNECTING, 255, 160, 0), "wifi is amber");
    CHECK(color_is(LED_STATUS_AP_MODE, 0, 80, 255), "ap is blue");
    CHECK(color_is(LED_STATUS_WIFI_OK_WAITING_SENDER, 0, 180, 180), "wait is cyan");
    CHECK(color_is(LED_STATUS_OK, 0, 255, 0), "ok is green");
    CHECK(color_is(LED_STATUS_ERROR, 255, 0, 0), "error is red");
    CHECK(color_is(LED_STATUS_OTA, 255, 0, 255), "ota is magenta");

    /* An out-of-range status falls back to BOOT (never a crash/black LED). */
    CHECK(color_is((led_status_t)99, 255, 255, 255), "invalid status -> boot colour");
    CHECK(strcmp(led_status_name((led_status_t)99), "?") == 0,
          "invalid status name is '?'");

    /* Patterns: BOOT is steady, the rest blink. */
    uint32_t period = 0, pulse = 0;
    led_status_pattern(LED_STATUS_BOOT, &period, &pulse);
    CHECK(period == 1000 && pulse == 1000, "boot is steady on");
    CHECK(!led_status_blinks(LED_STATUS_BOOT), "boot does not blink");

    led_status_pattern(LED_STATUS_OK, &period, &pulse);
    CHECK(period == 2500 && pulse == 120, "ok heartbeat 120/2500");
    CHECK(led_status_blinks(LED_STATUS_OK), "ok blinks");

    led_status_pattern(LED_STATUS_ERROR, &period, &pulse);
    CHECK(period == 1500 && pulse == 100, "error heartbeat 100/1500");
    led_status_pattern(LED_STATUS_OTA, &period, &pulse);
    CHECK(period == 1000 && pulse == 150, "ota heartbeat 150/1000");
    led_status_pattern(LED_STATUS_WIFI_CONNECTING, &period, &pulse);
    CHECK(period == 2000 && pulse == 100, "wifi heartbeat 100/2000");
    led_status_pattern(LED_STATUS_AP_MODE, &period, &pulse);
    CHECK(period == 2000 && pulse == 100, "ap heartbeat 100/2000");
    led_status_pattern(LED_STATUS_WIFI_OK_WAITING_SENDER, &period, &pulse);
    CHECK(period == 2000 && pulse == 100, "wait heartbeat 100/2000");

    /* Blink phase boundaries for a 100/2000 heartbeat. */
    led_status_output_t o;
    led_status_output(LED_STATUS_ERROR, 0, &o);
    CHECK(o.on && o.r == 255 && o.g == 0 && o.b == 0, "phase 0 is lit red");
    led_status_output(LED_STATUS_ERROR, 99, &o);
    CHECK(o.on, "phase pulse-1 is still lit");
    led_status_output(LED_STATUS_ERROR, 100, &o);
    CHECK(!o.on, "phase pulse is off");
    led_status_output(LED_STATUS_ERROR, 1499, &o);
    CHECK(!o.on, "end of period is off");
    led_status_output(LED_STATUS_ERROR, 1500, &o);
    CHECK(o.on, "wrap-around lights again");

    /* BOOT is always lit at any phase. */
    led_status_output(LED_STATUS_BOOT, 0, &o);
    CHECK(o.on, "boot phase 0 on");
    led_status_output(LED_STATUS_BOOT, 12345, &o);
    CHECK(o.on, "boot any phase on");

    /* Brightness scaling (round to nearest). */
    CHECK(led_status_scale(255, 16) == 16, "255 at b=16 -> 16");
    CHECK(led_status_scale(0, 16) == 0, "0 stays 0");
    CHECK(led_status_scale(128, 16) == 8, "128 at b=16 rounds to 8");
    CHECK(led_status_scale(255, 255) == 255, "full brightness is identity");
    CHECK(led_status_scale(255, 0) == 0, "zero brightness is dark");
    CHECK(led_status_scale(0, 0) == 0, "dark stays dark");
    /* A lit colour scaled by the default brightness stays non-zero. */
    CHECK(led_status_scale(255, 16) > 0, "default brightness is still visible");

    /* Names. */
    CHECK(strcmp(led_status_name(LED_STATUS_BOOT), "boot") == 0, "name boot");
    CHECK(strcmp(led_status_name(LED_STATUS_WIFI_CONNECTING), "wifi") == 0, "name wifi");
    CHECK(strcmp(led_status_name(LED_STATUS_AP_MODE), "ap") == 0, "name ap");
    CHECK(strcmp(led_status_name(LED_STATUS_WIFI_OK_WAITING_SENDER), "wait") == 0,
          "name wait");
    CHECK(strcmp(led_status_name(LED_STATUS_OK), "ok") == 0, "name ok");
    CHECK(strcmp(led_status_name(LED_STATUS_ERROR), "error") == 0, "name error");
    CHECK(strcmp(led_status_name(LED_STATUS_OTA), "ota") == 0, "name ota");

    /* parse is the inverse of name. */
    led_status_t parsed = LED_STATUS_COUNT;
    CHECK(led_status_parse("ok", &parsed) && parsed == LED_STATUS_OK, "parse ok");
    CHECK(led_status_parse("wait", &parsed) &&
              parsed == LED_STATUS_WIFI_OK_WAITING_SENDER,
          "parse wait");
    CHECK(!led_status_parse("nope", &parsed) && parsed == LED_STATUS_WIFI_OK_WAITING_SENDER,
          "parse rejects an unknown name and leaves the output");
    CHECK(!led_status_parse(NULL, &parsed), "parse rejects NULL");

    /* NULL safety. */
    led_status_color(LED_STATUS_OK, NULL, NULL, NULL);
    led_status_pattern(LED_STATUS_OK, NULL, NULL);
    led_status_output(LED_STATUS_OK, 0, NULL);
    CHECK(true, "NULL pointers are no-ops");

    printf("test_led_status: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
