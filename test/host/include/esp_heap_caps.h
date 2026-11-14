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

/* Largest free block, as the repair path reads it to clamp its buffer. The host
 * has no heap caps, so report "plenty" (SIZE_MAX) and let the compile-time bound
 * govern; the clamp arithmetic itself is unit-tested via
 * power_history_repair_keep_limit_for(). */
static inline size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return (size_t)-1;
}
