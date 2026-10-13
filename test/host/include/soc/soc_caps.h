/* Host-test shim: enough of soc/soc_caps.h for the light-sleep feature gate in
 * main/uart_sleep_window.c. The values are overridden on the CI command line. */
#pragma once

#ifndef SOC_PM_SUPPORT_LP_TIMER
#define SOC_PM_SUPPORT_LP_TIMER 1
#endif
#ifndef SOC_PM_SUPPORT_EXT_WAKEUP
#define SOC_PM_SUPPORT_EXT_WAKEUP 1
#endif
#ifndef SOC_PM_SUPPORT_GPIO_WAKEUP
#define SOC_PM_SUPPORT_GPIO_WAKEUP 0
#endif
