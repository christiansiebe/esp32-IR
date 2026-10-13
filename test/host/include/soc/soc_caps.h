/* Host-test shim: enough of soc/soc_caps.h for the light-sleep feature gate in
 * main/uart_sleep_window.c. Mirrors the real ESP32 caps: light sleep is
 * supported, SOC_PM_SUPPORT_LP_TIMER does not exist there. */
#pragma once

#ifndef SOC_LIGHT_SLEEP_SUPPORTED
#define SOC_LIGHT_SLEEP_SUPPORTED 1
#endif
