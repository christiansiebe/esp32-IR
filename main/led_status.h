/* Status LED: pure state -> colour + blink-phase mapping (host-testable).
 *
 * The ESP32-C6 receiver carries a single addressable WS2812 RGB LED. Its job is
 * a low-power, glanceable status: a dim, slow "heartbeat" pulse whose colour
 * names the current state. This module owns only the *decision* - which colour
 * and whether the LED is lit at a given moment - so it is pure C, deterministic
 * and unit-tested on the host (like runtime_config.c / health_policy.c). The
 * RMT/led_strip glue and the esp_timer tick live in led_indicator.c (IDF only).
 *
 * Power: the pulse is short and infrequent (a low duty cycle) and the colour is
 * scaled down by a caller-supplied brightness, so the LED adds only a tiny,
 * bounded load on top of the always-on Wi-Fi radio. The caller (led_indicator.c)
 * writes the strip only when the effective colour or the on/off phase changes,
 * so nothing is refreshed continuously.
 *
 * The LED only exists on the receiver (CONFIG_ESPNOW_ROLE_RECEIVER); the sender
 * is an ESP32 classic without one. */
#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdbool.h>
#include <stdint.h>

/* Receiver state shown on the LED. The order is the natural boot progression
 * and is used for indexing, so append new states before LED_STATUS_COUNT. */
typedef enum {
    LED_STATUS_BOOT = 0,        /* early boot, ~1 s: steady white */
    LED_STATUS_WIFI_CONNECTING, /* station associating to the home WLAN: amber */
    LED_STATUS_AP_MODE,         /* no WLAN configured, setup SoftAP only: blue */
    LED_STATUS_WIFI_OK_WAITING_SENDER, /* associated, no ESP-NOW data yet: cyan */
    LED_STATUS_OK,              /* ESP-NOW data flowing (normal): green */
    LED_STATUS_ERROR,           /* sender silent past the threshold, web guard
                                 * tripped or WLAN lost: red */
    LED_STATUS_OTA,             /* OTA in progress: magenta */
    LED_STATUS_COUNT,
} led_status_t;

/* One instant of the LED: the *base* colour (0..255 per channel, before
 * brightness scaling) and whether it is lit in this blink phase. A lit result
 * with a zero colour means "off"; the caller still treats `on == false` as off
 * regardless of the colour. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    bool on;
} led_status_output_t;

/* Fill *out with the base colour and lit/off phase for `status` at
 * `phase_ms` (a free-running monotonic millisecond counter, e.g. the time since
 * the status became active). Deterministic: the same inputs always give the same
 * output. No-op when out is NULL. */
void led_status_output(led_status_t status, uint32_t phase_ms,
                       led_status_output_t *out);

/* The base (fully lit) colour of `status`. No-op when any pointer is NULL. */
void led_status_color(led_status_t status, uint8_t *r, uint8_t *g, uint8_t *b);

/* The blink geometry of `status`: one pulse every `period_ms`, lit for the
 * first `pulse_ms` of each period. `period_ms == pulse_ms` means steady on (no
 * blinking). No-op when any pointer is NULL. */
void led_status_pattern(led_status_t status, uint32_t *period_ms,
                        uint32_t *pulse_ms);

/* True when `status` blinks (pulse < period). */
bool led_status_blinks(led_status_t status);

/* Scale a base colour component (0..255) by a global brightness (0..255):
 * component * brightness / 255, rounded to nearest. A brightness of 0 yields 0
 * (LED dark). */
uint8_t led_status_scale(uint8_t component, uint8_t brightness);

/* Short stable name for logs ("boot", "wifi", "ap", "wait", "ok", "error",
 * "ota"). Returns "?" for an out-of-range value. Never NULL. */
const char *led_status_name(led_status_t status);

/* Parse a status name (the inverse of led_status_name). Returns false, leaving
 * *out untouched, for an unknown name or a NULL argument. */
bool led_status_parse(const char *name, led_status_t *out);

/* How long the BOOT (white) state is shown after start, in milliseconds. */
#define LED_STATUS_BOOT_MS 1000u

#endif /* LED_STATUS_H */
