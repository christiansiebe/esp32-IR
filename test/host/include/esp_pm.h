/* Host-test shim: enough of esp_pm.h to syntax-check the enabled branch of
 * main/uart_sleep_window.c. Only types/constants are needed; the functions are
 * never linked (the CI step uses -fsyntax-only). */
#pragma once
#include "esp_shim.h"

typedef struct esp_pm_lock *esp_pm_lock_handle_t;

#define ESP_PM_NO_LIGHT_SLEEP 1

typedef struct {
    int max_freq_mhz;
    int min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;

esp_err_t esp_pm_lock_create(int type, int arg, const char *name,
                             esp_pm_lock_handle_t *out);
void esp_pm_lock_acquire(esp_pm_lock_handle_t h);
void esp_pm_lock_release(esp_pm_lock_handle_t h);
esp_err_t esp_pm_configure(const esp_pm_config_t *cfg);
