#include "led_status.h"

#include <stddef.h>
#include <string.h>

/* Base colours (before the caller's brightness scaling). Kept dim on purpose:
 * a WS2812 at full white draws tens of mA, which would dwarf the receiver's
 * ~0.43 W idle budget. */
static const uint8_t k_colors[LED_STATUS_COUNT][3] = {
    [LED_STATUS_BOOT] = {255, 255, 255}, /* white */
    [LED_STATUS_WIFI_CONNECTING] = {255, 160, 0}, /* amber */
    [LED_STATUS_AP_MODE] = {0, 80, 255}, /* blue */
    [LED_STATUS_WIFI_OK_WAITING_SENDER] = {0, 180, 180}, /* cyan */
    [LED_STATUS_OK] = {0, 255, 0}, /* green */
    [LED_STATUS_ERROR] = {255, 0, 0}, /* red */
    [LED_STATUS_OTA] = {255, 0, 255}, /* magenta */
};

/* Blink geometry: {period_ms, pulse_ms}. pulse == period is steady on. The
 * pulse is short and the period long, so the average current is a small
 * fraction of the lit current (a low-duty "heartbeat"). */
static const uint32_t k_pattern[LED_STATUS_COUNT][2] = {
    [LED_STATUS_BOOT] = {1000u, 1000u}, /* steady */
    [LED_STATUS_WIFI_CONNECTING] = {2000u, 100u},
    [LED_STATUS_AP_MODE] = {2000u, 100u},
    [LED_STATUS_WIFI_OK_WAITING_SENDER] = {2000u, 100u},
    [LED_STATUS_OK] = {2500u, 120u},
    [LED_STATUS_ERROR] = {1500u, 100u},
    [LED_STATUS_OTA] = {1000u, 150u},
};

static led_status_t clamp_status(led_status_t status)
{
    if (status < 0 || status >= LED_STATUS_COUNT) {
        return LED_STATUS_BOOT; /* safe, always-on default */
    }
    return status;
}

void led_status_color(led_status_t status, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r == NULL || g == NULL || b == NULL) {
        return;
    }
    led_status_t s = clamp_status(status);
    *r = k_colors[s][0];
    *g = k_colors[s][1];
    *b = k_colors[s][2];
}

void led_status_pattern(led_status_t status, uint32_t *period_ms,
                        uint32_t *pulse_ms)
{
    if (period_ms == NULL || pulse_ms == NULL) {
        return;
    }
    led_status_t s = clamp_status(status);
    *period_ms = k_pattern[s][0];
    *pulse_ms = k_pattern[s][1];
}

bool led_status_blinks(led_status_t status)
{
    led_status_t s = clamp_status(status);
    return k_pattern[s][1] < k_pattern[s][0];
}

void led_status_output(led_status_t status, uint32_t phase_ms,
                       led_status_output_t *out)
{
    if (out == NULL) {
        return;
    }
    led_status_t s = clamp_status(status);
    out->r = k_colors[s][0];
    out->g = k_colors[s][1];
    out->b = k_colors[s][2];
    uint32_t period = k_pattern[s][0];
    uint32_t pulse = k_pattern[s][1];
    if (period == 0u) {
        out->on = true; /* defensive; no pattern uses 0 */
        return;
    }
    out->on = (phase_ms % period) < pulse;
}

uint8_t led_status_scale(uint8_t component, uint8_t brightness)
{
    /* Round to nearest: (c*b + 127) / 255. */
    return (uint8_t)(((unsigned)component * (unsigned)brightness + 127u) / 255u);
}

const char *led_status_name(led_status_t status)
{
    switch (status) {
    case LED_STATUS_BOOT:
        return "boot";
    case LED_STATUS_WIFI_CONNECTING:
        return "wifi";
    case LED_STATUS_AP_MODE:
        return "ap";
    case LED_STATUS_WIFI_OK_WAITING_SENDER:
        return "wait";
    case LED_STATUS_OK:
        return "ok";
    case LED_STATUS_ERROR:
        return "error";
    case LED_STATUS_OTA:
        return "ota";
    default:
        return "?";
    }
}

bool led_status_parse(const char *name, led_status_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (led_status_t s = 0; s < LED_STATUS_COUNT; ++s) {
        if (strcmp(name, led_status_name(s)) == 0) {
            *out = s;
            return true;
        }
    }
    return false;
}
