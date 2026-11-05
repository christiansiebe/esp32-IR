#include "recovery_start.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "recovery_button.h"

/* The recovery button is honoured on both roles. The receiver has a soldered
 * button on the BOOT pin; the sender (which has no USB and is otherwise only
 * reachable over ESP-NOW) can carry the same button so a bad image or a wedged
 * config can be recovered locally instead of only via the relayed OTA path.
 * CONFIG_RECOVERY_BUTTON_GPIO = -1 still compiles the whole thing to no-op
 * stubs, e.g. for a board without a button or where GPIO0 is needed elsewhere. */
#if CONFIG_RECOVERY_BUTTON_GPIO >= 0

static const char *TAG = "recovery";

#define RECOVERY_POLL_MS 50u

#ifndef CONFIG_RECOVERY_BUTTON_RESTORE_MS
#define CONFIG_RECOVERY_BUTTON_RESTORE_MS 3000
#endif
#ifndef CONFIG_RECOVERY_BUTTON_ROLLBACK_MS
#define CONFIG_RECOVERY_BUTTON_ROLLBACK_MS 10000
#endif

static recovery_restore_fn s_restore_fn;

void recovery_set_restore_fn(recovery_restore_fn fn)
{
    s_restore_fn = fn;
}

static bool button_pressed(void)
{
    /* The BOOT button shorts the pin to ground. */
    return gpio_get_level((gpio_num_t)CONFIG_RECOVERY_BUTTON_GPIO) == 0;
}

esp_err_t recovery_button_force_rollback(void)
{
    ESP_LOGE(TAG, "Forcing rollback to the previous firmware");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only reached when the rollback could not be started. */
    ESP_LOGE(TAG, "Rollback not possible: %s", esp_err_to_name(err));
    return err;
}

esp_err_t recovery_button_check(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_RECOVERY_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Recovery button GPIO unavailable: %s", esp_err_to_name(err));
        return err;
    }

    recovery_state_t state;
    recovery_state_reset(&state);
    const uint32_t restore_ms = CONFIG_RECOVERY_BUTTON_RESTORE_MS;
    const uint32_t rollback_ms = CONFIG_RECOVERY_BUTTON_ROLLBACK_MS;

    if (button_pressed()) {
        ESP_LOGW(TAG, "Recovery button held: release after %u ms to restore the "
                      "config, keep holding to %u ms to roll back",
                 (unsigned)restore_ms, (unsigned)rollback_ms);
    }

    for (;;) {
        recovery_action_t action =
            recovery_evaluate(&state, (uint32_t)(esp_timer_get_time() / 1000),
                              button_pressed(), restore_ms, rollback_ms);
        if (action == RECOVERY_ROLLBACK) {
            return recovery_button_force_rollback();
        }
        if (action == RECOVERY_RESTORE) {
            ESP_LOGW(TAG, "Restoring the previous runtime config");
            if (s_restore_fn != NULL) {
                /* Restores the previous values and reboots; does not return. */
                s_restore_fn();
            }
            ESP_LOGE(TAG, "No config restore handler registered; continuing boot");
            return ESP_OK;
        }
        if (!state.active) {
            /* Not pressed: normal boot, stop watching. */
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(RECOVERY_POLL_MS));
    }
}

#else /* CONFIG_RECOVERY_BUTTON_GPIO < 0 */

void recovery_set_restore_fn(recovery_restore_fn fn) { (void)fn; }
esp_err_t recovery_button_check(void) { return ESP_OK; }
esp_err_t recovery_button_force_rollback(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
