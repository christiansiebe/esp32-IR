#pragma once
#include "esp_shim.h"

#include <stdarg.h>

/* Minimal stand-in for the ESP-IDF log hook API so boot_log.c compiles on the
 * host. The host test drives boot_log_capture_line() directly; the hook itself
 * is never installed here, but boot_log_init() calls esp_log_set_vprintf(), so
 * it must exist. */
typedef int (*vprintf_like_t)(const char *, va_list);

static inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t fn)
{
    (void)fn;
    return NULL;
}
