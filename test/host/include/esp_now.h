/* Host-test shim: enough of esp_now.h to syntax-check main/power_tuning.c's
 * power-save branch (the function is only referenced, never called, on the
 * host). */
#pragma once
#include "esp_shim.h"

esp_err_t esp_now_set_wake_window(uint16_t window);
