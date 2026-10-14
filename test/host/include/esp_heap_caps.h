/* Host-test shim for esp_heap_caps.h: the desktop has no PSRAM/heap caps, so
 * MALLOC_CAP_* are ignored and the allocation is a plain malloc. Used by the
 * ring-repair path in power_history.c, which the store tests exercise. */
#pragma once
#include "esp_shim.h"

#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_SPIRAM 0

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}
