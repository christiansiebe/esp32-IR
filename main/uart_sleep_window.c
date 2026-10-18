#include "uart_sleep_window.h"

/* CONFIG_ESPNOW_SENDER_LIGHT_SLEEP arrives via the IDF-injected sdkconfig.h and
 * SOC_LIGHT_SLEEP_SUPPORTED via soc_caps.h, exactly as in main.c. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#define UART_SLEEP_GAP_THRESHOLD_MS 50u
/* Must match UART_PORT_NUM in main.c; used only to peek at the RX buffer after
 * a sleep window to decide whether the window was clean or too long. */
#define UART_SLEEP_PORT_NUM UART_NUM_1

/* SOC_LIGHT_SLEEP_SUPPORTED is the capability that actually exists on the
 * ESP32; SOC_PM_SUPPORT_LP_TIMER does NOT exist there, so a guard on it made
 * the whole file compile to the no-op stub while the build still reported the
 * sleep profile. */
#if CONFIG_ESPNOW_SENDER_LIGHT_SLEEP && SOC_LIGHT_SLEEP_SUPPORTED

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#define UART_SLEEP_TAG "uart_sleep"

/* Keep the CPU awake for at least this long after the last ESP-NOW request, so
 * a history pull or OTA exchange is not starved by a sleep window. */
#define UART_SLEEP_HOLD_MS 1000u

static uart_sleep_ctrl_t s_ctrl;
static bool s_locked = true; /* NO_LIGHT_SLEEP is held by default */
static esp_pm_lock_handle_t s_no_light_sleep;
static volatile int64_t s_hold_until_us;
static volatile uint32_t s_slept_ms; /* real light-sleep time, fed by main.c */
static volatile bool s_disabled;     /* link fallback dropped light sleep */

/* CPU frequency pinned by the test console (0 = use the compile-time values).
 * The three esp_pm_configure() calls below would otherwise reset a pinned clock
 * back to CONFIG_ESPNOW_CPU_MAX_MHZ / 40 whenever the sleep toggle is used. */
static volatile int s_pm_max_override;
static volatile int s_pm_min_override;

/* Result of the last ESP-NOW wake-window setup, for diagnostics (the function
 * itself only has ESP_LOG, which the sender cannot easily read). */
static esp_err_t s_last_wake_err = ESP_ERR_INVALID_STATE;

static int sleep_pm_max(void)
{
    return s_pm_max_override > 0 ? s_pm_max_override : CONFIG_ESPNOW_CPU_MAX_MHZ;
}

static int sleep_pm_min(void)
{
    return s_pm_min_override > 0 ? s_pm_min_override : 40;
}

bool uart_sleep_init_idf(void)
{
    uart_sleep_init(&s_ctrl, uart_sleep_default_cfg());

    /* Light sleep is only allowed inside the verified quiet window, never while
     * the UART is carrying a telegram, so the lock starts held. */
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "uart_sleep",
                           &s_no_light_sleep) != ESP_OK) {
        return false;
    }

    /* Variable CPU clock (DFS) plus light sleep in the idle path. Without this
     * call light_sleep_enable stays 0 and the chip never sleeps. The ceiling is
     * shared with the receiver (CONFIG_ESPNOW_CPU_MAX_MHZ). */
    esp_pm_config_t pm = {
        .max_freq_mhz = sleep_pm_max(),
        .min_freq_mhz = sleep_pm_min(),
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&pm) != ESP_OK) {
        return false;
    }

    /* Wi-Fi modem sleep is set after esp_wifi_start() (see initialize_wifi_and_espnow):
     * calling esp_wifi_set_ps() here, before esp_wifi_init(), silently failed. */

    esp_pm_lock_acquire(s_no_light_sleep);
    /* Compiled in only when this real branch is active (not the stub), so the
     * CI can grep the built image to prove the sleep path is present. */
    ESP_LOGI(UART_SLEEP_TAG, "adaptive sleep path compiled in");
    return true;
}

/* Enable the ESP-NOW connectionless wake window. Must be called once the station
 * is associated: esp_now_set_wake_window() only takes effect at connected status
 * (per the IDF docs), and on an unassociated station the ESP-NOW module keeps
 * the radio on and the APB lock asserted, so DFS never steps down. Called from
 * the sender's WIFI_EVENT_STA_CONNECTED handler, so it re-applies on every
 * (re)association. No-op when the feature is compiled out. */
void uart_sleep_espnow_power_save_enable(void)
{
#if CONFIG_ESPNOW_SENDER_ESPNOW_POWER_SAVE
    /* Order matters: Espressif's own examples set the window first, then the
     * interval (see issue esp-idf#11714 / the Super-2024 swadge notes). */
    esp_err_t err = esp_now_set_wake_window(
        (uint16_t)CONFIG_ESPNOW_SENDER_ESPNOW_WAKE_WINDOW_MS);
    if (err == ESP_OK) {
        err = esp_wifi_connectionless_module_set_wake_interval(
            (uint16_t)CONFIG_ESPNOW_SENDER_ESPNOW_WAKE_INTERVAL_MS);
    }
    s_last_wake_err = err;
    if (err != ESP_OK) {
        ESP_LOGW(UART_SLEEP_TAG,
                 "Could not enable ESP-NOW power save (%s); the CPU may stay at "
                 "the DFS ceiling because the Wi-Fi APB lock is never released",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(UART_SLEEP_TAG, "ESP-NOW power save on: radio wakes every %d ms "
                                 "for %d ms, so DFS/light sleep can engage",
                 CONFIG_ESPNOW_SENDER_ESPNOW_WAKE_INTERVAL_MS,
                 CONFIG_ESPNOW_SENDER_ESPNOW_WAKE_WINDOW_MS);
    }
#endif
}

/* Undo the ESP-NOW modem sleep. The link fallback's whole point is to restore
 * the proven mode ("radio always on"), but disabling light sleep alone leaves
 * WIFI_PS_MIN_MODEM in place, so the radio keeps cycling and the link stays
 * broken - a modem-sleep image could never heal itself. Set WIFI_PS_NONE and
 * drop the wake window so the radio is awake for good. No-op when the feature is
 * compiled out. */
void uart_sleep_espnow_power_save_disable(void)
{
#if CONFIG_ESPNOW_SENDER_ESPNOW_POWER_SAVE
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) {
        (void)esp_now_set_wake_window(0);
    }
    s_last_wake_err = err;
    if (err != ESP_OK) {
        ESP_LOGW(UART_SLEEP_TAG,
                 "Could not disable Wi-Fi power save (%s); the radio may stay "
                 "in modem sleep", esp_err_to_name(err));
    } else {
        ESP_LOGW(UART_SLEEP_TAG,
                 "Wi-Fi power save off: radio stays awake (proven mode)");
    }
#endif
}

int uart_sleep_espnow_last_err(void)
{
    return (int)s_last_wake_err;
}

/* Acquire/release exactly once per transition: esp_pm_lock_acquire() is
 * reference-counted, so acquiring per byte would leak the count and pin the
 * CPU awake forever. */
static void uart_sleep_lock(bool lock)
{
    if (lock == s_locked) {
        return;
    }
    if (lock) {
        esp_pm_lock_acquire(s_no_light_sleep);
    } else {
        esp_pm_lock_release(s_no_light_sleep);
    }
    s_locked = lock;
}

void uart_sleep_note_uart_byte(uint64_t now_us, uint64_t last_byte_us, uint32_t received,
                               bool awaiting)
{
    (void)received;
    uart_sleep_note_byte(&s_ctrl, now_us, last_byte_us, UART_SLEEP_GAP_THRESHOLD_MS,
                         awaiting);
    if (!uart_sleep_should_request(&s_ctrl)) {
        /* Bytes are flowing: keep the CPU awake for the whole burst. */
        uart_sleep_lock(true);
    }
}

void uart_sleep_note_uart_idle(void)
{
    uart_sleep_burst_idle(&s_ctrl);
}

void uart_sleep_hold(uint32_t hold_ms)
{
    int64_t now = esp_timer_get_time();
    int64_t until = now + (int64_t)hold_ms * 1000;
    /* Refresh monotonically: a burst of requests extends the hold. */
    if (until > s_hold_until_us) {
        s_hold_until_us = until;
    }
}

void uart_sleep_maybe_sleep_idf(void)
{
    if (s_disabled) {
        return;
    }
    if (!uart_sleep_should_request(&s_ctrl)) {
        return;
    }
    /* Never sleep while an ESP-NOW request (history pull / OTA) is in flight. */
    if (uart_sleep_hold_active(s_hold_until_us, esp_timer_get_time())) {
        return;
    }
    uint32_t window_ms = s_ctrl.window_ms;
    if (window_ms == 0) {
        return;
    }

    /* Release the lock for exactly the adaptive window. With light sleep
     * enabled and all tasks blocked, tickless idle parks the CPU for it. */
    uint32_t before_ms = s_slept_ms;
    uart_sleep_lock(false);
    vTaskDelay(pdMS_TO_TICKS(window_ms));
    uart_sleep_lock(true);

    /* If the UART buffer is still empty after the window, nothing arrived while
     * we slept: the window was clean and may grow. If bytes are already pending,
     * the window was too long (a telegram started inside it) -> cut it. */
    size_t pending = 0;
    if (uart_get_buffered_data_len(UART_SLEEP_PORT_NUM, &pending) != ESP_OK) {
        pending = 0;
    }
    uint32_t slept_ms = s_slept_ms - before_ms;
    if (pending == 0) {
        uart_sleep_note_clean(&s_ctrl, slept_ms);
    } else {
        uart_sleep_note_short_sleep(&s_ctrl);
    }
}

/* main.c feeds the real light-sleep time here (its sleep-exit callback owns the
 * accumulation). Kept separate so the controller only grows a window it actually
 * tested instead of assuming every vTaskDelay parked the CPU. */
void uart_sleep_set_slept_ms(uint32_t ms) { s_slept_ms = ms; }

/* Link fallback: the peer stopped answering while we slept. Turn light sleep off
 * for good (a re-enable would flap) and keep the CPU awake. The DFS ceiling
 * stays; only the idle sleep goes away. */
void uart_sleep_disable_idf(void)
{
    if (s_disabled) {
        return;
    }
    s_disabled = true;
    esp_pm_config_t pm = {
        .max_freq_mhz = sleep_pm_max(),
        .min_freq_mhz = sleep_pm_min(),
        .light_sleep_enable = false,
    };
    (void)esp_pm_configure(&pm);
    uart_sleep_lock(true); /* hold NO_LIGHT_SLEEP so the idle path never parks */
    ESP_LOGW(UART_SLEEP_TAG,
             "light sleep disabled by the link fallback; CPU stays awake");
}

uint32_t uart_sleep_window_ms(void) { return s_ctrl.window_ms; }
uint32_t uart_sleep_min_gap_ms(void) { return s_ctrl.last_min_gap_ms; }
bool uart_sleep_hold_is_active(void)
{
    return uart_sleep_hold_active(s_hold_until_us, esp_timer_get_time());
}

void uart_sleep_set_enabled(bool enabled)
{
    if (enabled) {
        /* Clear the fallback latch and allow the idle path to park again. */
        s_disabled = false;
        esp_pm_config_t pm = {
            .max_freq_mhz = sleep_pm_max(),
            .min_freq_mhz = sleep_pm_min(),
            .light_sleep_enable = true,
        };
        (void)esp_pm_configure(&pm);
        uart_sleep_lock(true); /* lock is released only for the sleep window */
    } else {
        uart_sleep_disable_idf();
    }
}

bool uart_sleep_is_enabled(void) { return !s_disabled; }

bool uart_sleep_force_window(uint32_t ms, uint32_t *slept_ms_out)
{
    if (slept_ms_out != NULL) {
        *slept_ms_out = 0;
    }
    if (s_disabled || ms == 0 || ms > 1000u) {
        return false;
    }
    uint32_t before_ms = s_slept_ms;
    uart_sleep_lock(false);
    vTaskDelay(pdMS_TO_TICKS(ms));
    uart_sleep_lock(true);
    if (slept_ms_out != NULL) {
        *slept_ms_out = s_slept_ms - before_ms; /* real light-sleep time */
    }
    return true;
}

/* Test console: remember a pinned CPU range so the sleep toggle re-applies it
 * instead of resetting the clock to the compile-time values. max <= 0 clears
 * the override. */
void uart_sleep_set_pm_override(int max_mhz, int min_mhz)
{
    s_pm_max_override = max_mhz;
    s_pm_min_override = min_mhz;
}

#else /* feature disabled or unsupported target: the glue is a no-op */

bool uart_sleep_init_idf(void) { return false; }
void uart_sleep_espnow_power_save_enable(void) {}
void uart_sleep_espnow_power_save_disable(void) {}
int uart_sleep_espnow_last_err(void) { return ESP_ERR_INVALID_STATE; }
void uart_sleep_note_uart_byte(uint64_t now_us, uint64_t last_byte_us, uint32_t received,
                               bool awaiting)
{
    (void)now_us;
    (void)last_byte_us;
    (void)received;
    (void)awaiting;
}
void uart_sleep_note_uart_idle(void) {}
void uart_sleep_hold(uint32_t hold_ms) { (void)hold_ms; }
void uart_sleep_maybe_sleep_idf(void) {}
void uart_sleep_set_slept_ms(uint32_t ms) { (void)ms; }
void uart_sleep_disable_idf(void) {}
void uart_sleep_set_enabled(bool enabled) { (void)enabled; }
bool uart_sleep_is_enabled(void) { return false; }
bool uart_sleep_force_window(uint32_t ms, uint32_t *slept_ms_out)
{
    (void)ms;
    if (slept_ms_out != NULL) {
        *slept_ms_out = 0;
    }
    return false;
}
void uart_sleep_set_pm_override(int max_mhz, int min_mhz)
{
    (void)max_mhz;
    (void)min_mhz;
}
uint32_t uart_sleep_window_ms(void) { return 0; }
uint32_t uart_sleep_min_gap_ms(void) { return 0; }
bool uart_sleep_hold_is_active(void) { return false; }

#endif
