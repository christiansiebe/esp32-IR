#include "power_tuning.h"

#include "esp_err.h"
#include "esp_log.h"

/* CONFIG_ESPNOW_RX_POWER_SAVE arrives via the IDF-injected sdkconfig.h and is
 * only defined for the sender (Kconfig `depends on ESPNOW_ROLE_SENDER`). */
#if CONFIG_ESPNOW_RX_POWER_SAVE

#include "esp_now.h"
#include "esp_wifi.h"

static const char *TAG = "power_tuning";

bool espnow_ps_apply(uint16_t interval_ms, uint16_t window_ms)
{
    uint16_t interval = 0;
    uint16_t window = 0;
    espnow_ps_clamp(interval_ms, window_ms, &interval, &window);

    esp_err_t interval_err =
        esp_wifi_connectionless_module_set_wake_interval(interval);
    esp_err_t window_err = esp_now_set_wake_window(window);
    if (interval_err == ESP_OK && window_err == ESP_OK) {
        ESP_LOGI(TAG, "ESP-NOW RX power save: interval %u ms, window %u ms",
                 (unsigned)interval, (unsigned)window);
        return true;
    }
    ESP_LOGW(TAG, "ESP-NOW RX power save not applied (interval=%s, window=%s)",
             esp_err_to_name(interval_err), esp_err_to_name(window_err));
    return false;
}

#else /* feature disabled or other role: the glue is a no-op */

bool espnow_ps_apply(uint16_t interval_ms, uint16_t window_ms)
{
    (void)interval_ms;
    (void)window_ms;
    return false;
}

#endif
