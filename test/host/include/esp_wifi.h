/* Host-test shim: enough of esp_wifi.h to syntax-check the sleep-window glue. */
#pragma once
#include "esp_shim.h"

typedef enum {
    WIFI_PS_NONE = 0,
    WIFI_PS_MIN_MODEM = 1,
    WIFI_PS_MAX_MODEM = 2,
} wifi_ps_type_t;

esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);

/* ESP-NOW connectionless-module wake interval (see uart_sleep_window.c). */
esp_err_t esp_wifi_connectionless_module_set_wake_interval(uint16_t interval);
