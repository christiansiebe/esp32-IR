/* Host-test shim: enough of esp_now.h to syntax-check the sleep-window glue's
 * ESP-NOW power-save call. Types/constants only; the function is never linked
 * (the CI step uses -fsyntax-only). */
#pragma once
#include "esp_shim.h"

esp_err_t esp_now_set_wake_window(uint16_t window);
