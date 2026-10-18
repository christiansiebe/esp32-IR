/* Host-test shim: enough of the ESP-IDF/FreeRTOS surface to compile and link
 * main/power_history.c on a normal desktop. Only the pure aggregation helpers
 * are exercised by the tests; the storage paths are never reached because
 * power_history_init() is not called and g_history_file stays NULL. */
#ifndef HOST_ESP_SHIM_H
#define HOST_ESP_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(x) (x)

typedef int BaseType_t;
typedef unsigned int UBaseType_t;

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0

typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;

/* Optional host log sink. Production code logs via ESP_LOGx; on the host the
 * macros forward the formatted message here so a test can assert on a specific
 * line (e.g. the "Persistent history ready: N of ..." boot line). The weak
 * definition means tests that do not set host_log_sink keep the old no-op
 * behaviour. */
typedef void (*host_log_fn)(const char *tag, const char *msg);
__attribute__((weak)) host_log_fn host_log_sink;
static inline void host_log_emit(const char *tag, const char *fmt, ...)
{
    if (host_log_sink == 0) {
        return;
    }
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    host_log_sink(tag, buf);
}

#define ESP_LOGE(tag, ...) host_log_emit(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log_emit(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log_emit(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) host_log_emit(tag, __VA_ARGS__)

static inline const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "err";
}

/* Real (pthread-backed) FreeRTOS primitives, so the reboot-persistence host
 * test can drive the actual store: a writer task draining a queue and a mutex
 * guarding it. Tests that never call power_history_init() do not touch them. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned char *buf;
    unsigned len, size, head, tail, count;
} host_queue_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(*m));
    if (m != NULL) {
        pthread_mutex_init(m, NULL);
    }
    return m;
}
static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t)
{
    (void)t;
    if (s == NULL) {
        return pdFALSE;
    }
    return pthread_mutex_lock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
}
static inline int xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s == NULL) {
        return pdFALSE;
    }
    return pthread_mutex_unlock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
}

static inline QueueHandle_t xQueueCreate(unsigned len, unsigned size)
{
    if (len == 0 || size == 0) {
        return NULL;
    }
    host_queue_t *q = (host_queue_t *)calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    q->buf = (unsigned char *)calloc(len, size);
    q->len = len;
    q->size = size;
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    return q;
}
static inline int xQueueSend(QueueHandle_t q, const void *item, uint32_t t)
{
    (void)t;
    if (q == NULL) {
        return pdFALSE;
    }
    host_queue_t *h = (host_queue_t *)q;
    pthread_mutex_lock(&h->m);
    int ok = pdFALSE;
    if (h->count < h->len) {
        memcpy(h->buf + (size_t)h->tail * h->size, item, h->size);
        h->tail = (h->tail + 1u) % h->len;
        ++h->count;
        ok = pdTRUE;
        pthread_cond_signal(&h->c);
    }
    pthread_mutex_unlock(&h->m);
    return ok;
}
static inline int xQueueReceive(QueueHandle_t q, void *item, uint32_t t)
{
    if (q == NULL) {
        return pdFALSE;
    }
    host_queue_t *h = (host_queue_t *)q;
    pthread_mutex_lock(&h->m);
    if (t == portMAX_DELAY) {
        while (h->count == 0) {
            pthread_cond_wait(&h->c, &h->m);
        }
    } else {
        /* No timeout support on the host: poll the count while yielding. Only
         * the reboot-persistence test takes this path, with a generous retry
         * budget, so a bounded busy-wait is fine and keeps the shim portable
         * (no clock_gettime / _POSIX_C_SOURCE dependency for the other tests). */
        unsigned spins = t == 0 ? 0 : t * 1000u;
        while (h->count == 0 && spins > 0) {
            pthread_mutex_unlock(&h->m);
            sched_yield();
            pthread_mutex_lock(&h->m);
            --spins;
        }
    }
    int ok = pdFALSE;
    if (h->count > 0) {
        memcpy(item, h->buf + (size_t)h->head * h->size, h->size);
        h->head = (h->head + 1u) % h->len;
        --h->count;
        ok = pdTRUE;
    }
    pthread_mutex_unlock(&h->m);
    return ok;
}
typedef struct {
    void (*fn)(void *);
    void *arg;
} host_task_t;

static void *host_task_trampoline(void *p)
{
    host_task_t *t = (host_task_t *)p;
    t->fn(t->arg);
    free(t);
    return NULL;
}

static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                                     void *arg, unsigned prio, void *handle)
{
    (void)name; (void)stack; (void)prio; (void)handle;
    host_task_t *t = (host_task_t *)malloc(sizeof(*t));
    if (t == NULL) {
        return pdFALSE;
    }
    t->fn = fn;
    t->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, host_task_trampoline, t) != 0) {
        free(t);
        return pdFALSE;
    }
    pthread_detach(th);
    return pdPASS;
}

static inline void portENTER_CRITICAL(portMUX_TYPE *m) { (void)m; }
static inline void portEXIT_CRITICAL(portMUX_TYPE *m) { (void)m; }

/* Only the syntax-check of the sleep glue calls this; real wait behaviour is
 * not exercised on the host. */
static inline void vTaskDelay(uint32_t ticks) { (void)ticks; }
static inline void vTaskDelete(void *task) { (void)task; }

typedef struct {
    const char *partition_label;
    const char *base_path;
    size_t max_files;
    bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;

static inline esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *c)
{
    /* Report success so power_history_init() takes the file-backed path. The
     * host has no SPIFFS, but the driver only does plain fopen() on
     * base_path/file, which the reboot-persistence host test provisions under
     * POWER_HISTORY_MOUNT_PATH. Tests that never call init() are unaffected. */
    (void)c;
    return ESP_OK;
}

#endif /* HOST_ESP_SHIM_H */
