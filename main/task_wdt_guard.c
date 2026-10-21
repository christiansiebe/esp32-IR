#include "task_wdt_guard.h"

#include "esp_log.h"
#include "esp_task_wdt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "task_wdt";

static bool s_armed;
static bool s_panic;
static uint32_t s_timeout_ms;
static uint32_t s_subscribed;

/* Mirror the IDF's own idle-task monitoring so reconfiguring the watchdog does
 * not silently change which idle cores are watched. */
static uint32_t idle_core_mask(void)
{
    uint32_t mask = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    mask |= 1u << 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 && !CONFIG_FREERTOS_UNICORE
    mask |= 1u << 1;
#endif
    return mask;
}

esp_err_t task_wdt_guard_init(void)
{
    if (s_armed) {
        return ESP_OK;
    }
    task_wdt_policy_t policy;
    task_wdt_policy_defaults(&policy);
    if (!task_wdt_policy_valid(&policy)) {
        ESP_LOGE(TAG, "TWDT policy is inconsistent; not arming");
        return ESP_ERR_INVALID_ARG;
    }

    esp_task_wdt_config_t config = {
        .timeout_ms = policy.timeout_ms,
        .idle_core_mask = idle_core_mask(),
        .trigger_panic = policy.panic,
    };
    /* With CONFIG_ESP_TASK_WDT_INIT=y the IDF has already created the TWDT
     * during startup, so calling esp_task_wdt_init() would only fail with
     * ESP_ERR_INVALID_STATE and emit a spurious "TWDT already initialized"
     * ERROR into the boot log. Adopt the existing handle with our generous
     * timeout + panic instead of leaving the default 5 s / no-panic policy that
     * would reset a healthy worker. Only when auto-init is disabled do we create
     * the watchdog ourselves. The compile-time flag is authoritative: there is
     * no query API that reports initialisation without logging an error. */
#if CONFIG_ESP_TASK_WDT_INIT
    esp_err_t err = esp_task_wdt_reconfigure(&config);
#else
    esp_err_t err = esp_task_wdt_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_reconfigure(&config);
    }
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure the task watchdog: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_armed = true;
    s_panic = policy.panic;
    s_timeout_ms = policy.timeout_ms;
    ESP_LOGI(TAG, "Task watchdog armed: timeout %u ms, panic %s",
             (unsigned)s_timeout_ms, s_panic ? "on" : "off");
    return ESP_OK;
}

bool task_wdt_guard_subscribe(const char *worker_name)
{
    if (!s_armed) {
        return false;
    }
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_OK) {
        ++s_subscribed;
        ESP_LOGI(TAG, "worker '%s' subscribed to the task watchdog",
                 worker_name != NULL ? worker_name : "?");
        return true;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        /* Already subscribed (a re-init or a double call): fine. */
        return true;
    }
    ESP_LOGW(TAG, "worker '%s' could not subscribe to the task watchdog: %s",
             worker_name != NULL ? worker_name : "?", esp_err_to_name(err));
    return false;
}

bool task_wdt_guard_feed(void)
{
    if (!s_armed) {
        return false;
    }
    return esp_task_wdt_reset() == ESP_OK;
}

void task_wdt_guard_get_status(task_wdt_guard_status_t *out)
{
    if (out == NULL) {
        return;
    }
    out->armed = s_armed;
    out->panic = s_panic;
    out->timeout_ms = s_timeout_ms;
    out->subscribed = s_subscribed;
}
