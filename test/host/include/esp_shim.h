/* Host-test shim: enough of the ESP-IDF/FreeRTOS surface to compile and link
 * main/power_history.c on a normal desktop. Only the pure aggregation helpers
 * are exercised by the tests; the storage paths are never reached because
 * power_history_init() is not called and g_history_file stays NULL. */
#ifndef HOST_ESP_SHIM_H
#define HOST_ESP_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)

static inline const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "err";
}

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return pdTRUE; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
static inline QueueHandle_t xQueueCreate(unsigned len, unsigned size) { (void)len; (void)size; return (void *)1; }
static inline int xQueueSend(QueueHandle_t q, const void *item, uint32_t t) { (void)q; (void)item; (void)t; return pdTRUE; }
static inline int xQueueReceive(QueueHandle_t q, void *item, uint32_t t) { (void)q; (void)item; (void)t; return pdFALSE; }
static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                                     void *arg, unsigned prio, void *handle)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
    return pdPASS;
}

static inline void portENTER_CRITICAL(portMUX_TYPE *m) { (void)m; }
static inline void portEXIT_CRITICAL(portMUX_TYPE *m) { (void)m; }

typedef struct {
    const char *partition_label;
    const char *base_path;
    size_t max_files;
    bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;

static inline esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *c)
{
    (void)c;
    return ESP_FAIL;
}

#endif /* HOST_ESP_SHIM_H */
