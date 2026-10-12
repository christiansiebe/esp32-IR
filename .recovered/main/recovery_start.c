#include "recovery_start.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "recovery_button.h"

static const char *TAG = "recovery";

/* Only the receiver carries the recovery button. */
#if !CONFIG_ESPNOW_ROLE_SENDER && CONFIG_RECOVERY_BUTTON_GPIO >= 0

#define RECOVERY_POLL_MS 50u

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
    const uint32_t hold_ms = CONFIG_RECOVERY_BUTTON_HOLD_MS;

    if (button_pressed()) {
        ESP_LOGW(TAG, "Recovery button held; keep holding to roll back");
    }

    for (;;) {
        if (recovery_evaluate(&state, (uint32_t)(esp_timer_get_time() / 1000),
                              button_pressed(), hold_ms) == RECOVERY_FORCE) {
            return recovery_button_force_rollback();
        }
        if (!state.active) {
            /* Not pressed: normal boot, stop watching. */
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(RECOVERY_POLL_MS));
    }
}

#else

esp_err_t recovery_button_check(void) { return ESP_OK; }
esp_err_t recovery_button_force_rollback(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
