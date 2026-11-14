#include "rwdt_guard.h"

#include "sdkconfig.h"

#if CONFIG_ESPNOW_RWDT_BACKSTOP

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "soc/rtc.h"
#include "hal/wdt_hal.h"

#include "rwdt_policy.h"

static const char *TAG = "rwdt";

static bool s_armed;
static uint32_t s_timeout_ms;
static uint32_t s_ticks;
static uint32_t s_feeds;

/* The feed is called from several tasks (the HTTP server, the history
 * backfill worker, the UART worker). The write-protect/feed/enable sequence
 * must not interleave, or one task's enable would make the next task's feed
 * silently ineffective. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t rwdt_guard_init(void)
{
    if (s_armed) {
        return ESP_OK;
    }

    uint32_t slow_hz = rtc_clk_slow_freq_get_hz();
    uint32_t timeout_ms = rwdt_clamp_timeout_ms(CONFIG_ESPNOW_RWDT_TIMEOUT_MS);
    uint32_t ticks = rwdt_timeout_ticks(timeout_ms, slow_hz);
    if (ticks == 0) {
        ESP_LOGW(TAG, "RWDT backstop not armed (slow clock %u Hz)", (unsigned)slow_hz);
        return ESP_ERR_INVALID_STATE;
    }

    /* Stage 0 is special on the C6: the hardware applies an implicit
     * multiplier of 2^(1 + EFUSE_WDT_DELAY_SEL) to the register value, so
     * rwdt_timeout_ticks() already rounded up to a safe multiple of 16. */
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_config_stage(&ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&ctx);
    wdt_hal_write_protect_enable(&ctx);

    s_armed = true;
    s_timeout_ms = timeout_ms;
    s_ticks = ticks;
    ESP_LOGI(TAG, "RWDT backstop armed: stage0 system reset at %u ms (%u ticks @ %u Hz)",
             (unsigned)timeout_ms, (unsigned)ticks, (unsigned)slow_hz);
    return ESP_OK;
}

void rwdt_guard_feed(void)
{
    if (!s_armed) {
        return;
    }
    /* The RWDT config registers are write-protected; the feed register is only
     * writable while protection is lifted. This mirrors esp_task_wdt's feed
     * (task_wdt_impl_esp_timer.c): without the unlock the feed would silently
     * do nothing and the hardware would reset the chip every timeout. */
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    portENTER_CRITICAL(&s_lock);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_feed(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    ++s_feeds;
    portEXIT_CRITICAL(&s_lock);
}

void rwdt_guard_get_status(rwdt_guard_status_t *out)
{
    if (out == NULL) {
        return;
    }
    out->armed = s_armed;
    out->timeout_ms = s_timeout_ms;
    out->ticks = s_ticks;
    out->feeds = s_feeds;
}

#else /* !CONFIG_ESPNOW_RWDT_BACKSTOP: inert on the other targets */

#include <string.h>

esp_err_t rwdt_guard_init(void) { return ESP_OK; }
void rwdt_guard_feed(void) {}
void rwdt_guard_get_status(rwdt_guard_status_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

#endif /* CONFIG_ESPNOW_RWDT_BACKSTOP */
