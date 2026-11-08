/* Status-LED hardware glue (receiver, ESP32-C6).
 *
 * Owns the single addressable WS2812 RGB LED: it creates the RMT/led_strip
 * driver, decides the colour + blink phase via the pure led_status module, and
 * writes the strip *only when the effective colour or the on/off phase actually
 * changes* (no continuous refresh, no animation loop). Everything is driven by
 * one low-frequency esp_timer tick, so the CPU stays idle between pulses.
 *
 * The caller (main.c) reports the current receiver state with
 * led_indicator_set_status(); the module turns that into the LED output. A
 * debug override (led_indicator_set_override) forces one state for bench work;
 * led_indicator_apply_config() (re)creates or releases the driver from the
 * persisted runtime_config_t LED knobs.
 *
 * The whole module is compiled out on the sender and on targets without RMT
 * (see led_indicator.c), so those builds carry no extra code. */
#ifndef LED_INDICATOR_H
#define LED_INDICATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "led_status.h"

/* Create/refresh the driver from the LED knobs (LED on/off, GPIO, brightness)
 * and start the blink tick. Safe to call repeatedly: it releases the driver
 * when the LED is disabled or gpio is 0 (no idle current). */
void led_indicator_apply_config(bool enabled, uint8_t gpio, uint8_t brightness);

/* Report the current receiver state. The colour only changes on a real state
 * transition, so calling this from a hot path is cheap. */
void led_indicator_set_status(led_status_t status);

/* Force one state regardless of the automatic status ("set led-status <name>"),
 * or LED_STATUS_COUNT to return to automatic. Persisted only in RAM. */
void led_indicator_set_override(led_status_t override_status);

/* Current automatic status (for the console/diagnostics). */
led_status_t led_indicator_status(void);

/* True when the RMT driver is currently created (LED enabled and gpio != 0). */
bool led_indicator_is_active(void);

#endif /* LED_INDICATOR_H */
