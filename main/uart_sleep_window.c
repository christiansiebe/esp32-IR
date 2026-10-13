#include "uart_sleep_window.h"

/* CONFIG_ESPNOW_SENDER_LIGHT_SLEEP and the SOC_PM_SUPPORT_* macros arrive via
 * the IDF-injected sdkconfig.h / soc_caps.h, exactly as in main.c. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#define UART_SLEEP_GAP_THRESHOLD_MS 50u
/* Must match UART_PORT_NUM in main.c; used only to peek at the RX buffer after
 * a sleep window to decide whether the window was clean or too long. */
#define UART_SLEEP_PORT_NUM UART_NUM_1

#if CONFIG_ESPNOW_SENDER_LIGHT_SLEEP && SOC_PM_SUPPORT_LP_TIMER && \
    (SOC_PM_SUPPORT_EXT_WAKEUP || SOC_PM_SUPPORT_GPIO_WAKEUP)

#include "driver/uart.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_wifi.h"

/* Keep the CPU awake for at least this long after the last ESP-NOW request, so
 * a history pull or OTA exchange is not starved by a sleep window. */
#define UART_SLEEP_HOLD_MS 1000u

static uart_sleep_ctrl_t s_ctrl;
static bool s_locked = true; /* NO_LIGHT_SLEEP is held by default */
static esp_pm_lock_handle_t s_no_light_sleep;
static volatile int64_t s_hold_until_us;

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
     * call light_sleep_enable stays 0 and the chip never sleeps. */
    esp_pm_config_t pm = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&pm) != ESP_OK) {
        return false;
    }

    /* Wi-Fi modem sleep releases the APB_MAX lock the Wi-Fi driver holds while
     * both interfaces are up; without it the CPU could never enter light sleep. */
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_pm_lock_acquire(s_no_light_sleep);
    return true;
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
    if (pending == 0) {
        uart_sleep_note_clean(&s_ctrl);
    } else {
        uart_sleep_note_short_sleep(&s_ctrl);
    }
}

uint32_t uart_sleep_window_ms(void) { return s_ctrl.window_ms; }
uint32_t uart_sleep_min_gap_ms(void) { return s_ctrl.last_min_gap_ms; }
bool uart_sleep_hold_is_active(void)
{
    return uart_sleep_hold_active(s_hold_until_us, esp_timer_get_time());
}

#else /* feature disabled or unsupported target: the glue is a no-op */

bool uart_sleep_init_idf(void) { return false; }
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
uint32_t uart_sleep_window_ms(void) { return 0; }
uint32_t uart_sleep_min_gap_ms(void) { return 0; }
bool uart_sleep_hold_is_active(void) { return false; }

#endif
