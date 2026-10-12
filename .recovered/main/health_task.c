#include "health_task.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "health_policy.h"

static const char *TAG = "health";

#define HEALTH_TASK_STACK    3072u
#define HEALTH_TASK_PRIORITY 2u
#define HEALTH_POLL_MS       5000u

static health_policy_t g_policy;
static health_state_t g_state;
static volatile uint32_t g_last_rx_ms;
static TaskHandle_t g_health_task;

void health_task_note_rx(void)
{
    g_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

static void health_worker(void *arg)
{
    (void)arg;
    /* Register with the task watchdog so a wedged health loop also reboots
     * instead of silently disappearing. */
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(HEALTH_POLL_MS));

        uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
        health_reason_t reason = health_evaluate(&g_policy, &g_state, uptime_ms,
                                                 g_last_rx_ms, free_heap);
        if (reason != HEALTH_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            /* On a still-unconfirmed image this rolls back; on a confirmed one
             * it is a plain reboot. */
            boot_confirm_fail(health_reason_name(reason));
        }
    }
}

esp_err_t health_task_start(void)
{
    health_policy_defaults(&g_policy);
    health_state_reset(&g_state);
    g_last_rx_ms = 0;
    if (xTaskCreate(health_worker, "health", HEALTH_TASK_STACK, NULL,
                    HEALTH_TASK_PRIORITY, &g_health_task) != pdPASS) {
        ESP_LOGE(TAG, "Could not start health watchdog task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
