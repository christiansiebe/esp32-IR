#pragma once

/* Small, pure helpers for the power/robustness knobs, kept out of main.c so the
 * decision logic can be host-tested without the ESP-IDF (test/test_power_tuning.c)
 * and syntax-checked in CI. The ESP-NOW glue itself lives in power_tuning.c. */

#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"

/* Kconfig symbols that may be absent (the other role, or the option is off).
 * Defaults are harmless so the call sites in main.c need no #if. */
#ifndef CONFIG_ESPNOW_PS_INTERVAL_MS
#define CONFIG_ESPNOW_PS_INTERVAL_MS 0
#endif
#ifndef CONFIG_ESPNOW_PS_WINDOW_MS
#define CONFIG_ESPNOW_PS_WINDOW_MS 0
#endif
#ifndef CONFIG_ESPNOW_AP_BEACON_INTERVAL
#define CONFIG_ESPNOW_AP_BEACON_INTERVAL 100
#endif

/* UART RX source clock.
 *
 * On the ESP32 UART_SCLK_DEFAULT is the APB clock, and the IDF UART driver
 * programs the baud-rate divider once, from that clock, without reprogramming
 * it when DFS changes the APB frequency. DFS can therefore halve the APB clock
 * mid-telegram and make the receiver sample at half the baud rate, which shows
 * up as checksum errors. REF_TICK is a fixed 1 MHz clock that does not track the
 * APB clock, so the divider stays valid across DFS steps; the IDF's own UART
 * console uses REF_TICK for exactly this reason (uart_vfs.c).
 *
 * With power management disabled the APB clock is fixed, so the default APB
 * source is kept (identical to the previous behaviour). */
#if defined(CONFIG_PM_ENABLE) && CONFIG_PM_ENABLE
#define UART_RX_PM_ENABLED true
#else
#define UART_RX_PM_ENABLED false
#endif

static inline uart_sclk_t uart_pick_rx_source_clk(bool pm_enabled)
{
    return pm_enabled ? UART_SCLK_REF_TICK : UART_SCLK_DEFAULT;
}

/* ESP-NOW connectionless-module power save.
 *
 * The connectionless module (ESP-NOW) listens continuously by default. A wake
 * window shorter than the wake interval makes it listen only for `window` out of
 * every `interval` ms. Espressif recommends a window of about 70% of the
 * interval. A window at or above the interval would mean "listen all the time",
 * so it is clamped to interval - 1; the interval is clamped to the API range
 * 1..65535. */
static inline void espnow_ps_clamp(uint16_t interval_ms, uint16_t window_ms,
                                   uint16_t *out_interval, uint16_t *out_window)
{
    if (interval_ms == 0) {
        interval_ms = 1;
    }
    if (window_ms >= interval_ms) {
        window_ms = (uint16_t)(interval_ms - 1);
    }
    *out_interval = interval_ms;
    *out_window = window_ms;
}

/* SoftAP beacon interval, in TU (1 TU = 1024 us). The Wi-Fi API requires a
 * multiple of 100 in the range 100..60000; the default is 100. The requested
 * value is clamped into range and rounded down to a multiple of 100. */
static inline uint16_t ap_beacon_interval_clamp(uint32_t requested_tu)
{
    uint32_t tu = requested_tu;
    if (tu < 100) {
        tu = 100;
    }
    if (tu > 60000) {
        tu = 60000;
    }
    tu = (tu / 100) * 100;
    return (uint16_t)tu;
}

/* Apply the ESP-NOW RX power save (interval/window). Returns true when it was
 * applied. A no-op returning false unless CONFIG_ESPNOW_RX_POWER_SAVE is set
 * (a sender-only Kconfig option), so the call site is role-independent. */
bool espnow_ps_apply(uint16_t interval_ms, uint16_t window_ms);
