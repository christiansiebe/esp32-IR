#include "boot_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BOOT_LOG_CAPACITY 6144

static char s_ring[BOOT_LOG_CAPACITY];
static size_t s_head;   /* next write index */
static size_t s_count;  /* valid bytes in the ring (<= capacity) */
static size_t s_total;  /* bytes seen since boot, including overwritten */
static bool s_active;

static SemaphoreHandle_t s_mutex;
static bool s_hook_installed;
static vprintf_like_t s_orig_vprintf;

static void boot_log_push(const char *data, size_t len)
{
    if (s_mutex == NULL) {
        return;
    }
    /* Normal log calls are the only writers; a brief mutex hold is fine. The
     * panic path does not go through this hook, so it can never be deadlocked
     * by a stalled reader. A reader can block logging for the duration of its
     * copy, which is why reads are rare and small. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < len; ++i) {
        s_ring[s_head] = data[i];
        s_head = (s_head + 1) % BOOT_LOG_CAPACITY;
        if (s_count < BOOT_LOG_CAPACITY) {
            ++s_count;
        }
        ++s_total;
    }
    xSemaphoreGive(s_mutex);
}

static int boot_log_vprintf(const char *fmt, va_list args)
{
    /* Always emit to the real console first: the hook must never swallow log
     * output (the serial monitor and the panic path depend on it). */
    va_list copy;
    va_copy(copy, args);
    int written = s_orig_vprintf(fmt, copy);
    va_end(copy);

    if (!s_active || written <= 0) {
        return written;
    }

    /* Re-render into a small stack buffer for the ring. A line longer than the
     * buffer is truncated in the captured copy only. */
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0) {
        return written;
    }
    size_t take = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    boot_log_push(line, take);
    return written;
}

void boot_log_init(void)
{
    if (s_hook_installed) {
        return;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return; /* no RAM for the mutex: leave the log on UART only */
    }
    s_active = true;
    s_orig_vprintf = esp_log_set_vprintf(boot_log_vprintf);
    if (s_orig_vprintf == NULL) {
        /* esp_log_set_vprintf returns the previous hook; NULL means there was
         * none, which is normal. Keep our hook installed. */
        s_orig_vprintf = vprintf;
    }
    s_hook_installed = true;
}

size_t boot_log_read(char *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }
    if (!s_hook_installed) {
        out[0] = '\0';
        return 0;
    }
    size_t n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_count > 0) {
        size_t start = (s_head + BOOT_LOG_CAPACITY - s_count) % BOOT_LOG_CAPACITY;
        n = s_count < (max - 1) ? s_count : (max - 1);
        for (size_t i = 0; i < n; ++i) {
            out[i] = s_ring[(start + i) % BOOT_LOG_CAPACITY];
        }
    }
    out[n] = '\0';
    xSemaphoreGive(s_mutex);
    return n;
}

size_t boot_log_total(void)
{
    return s_total;
}
