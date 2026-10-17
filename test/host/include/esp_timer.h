/* Host-test shim: enough of esp_timer.h to syntax-check and host-link the
 * sleep-window glue. esp_timer_get_time() returns a monotonic microsecond
 * count; only the syntax-check step compiles the glue, so it need not be real. */
#pragma once
#include "esp_shim.h"

int64_t esp_timer_get_time(void);
