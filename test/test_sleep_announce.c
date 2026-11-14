/* Host test for the sender's sleep-state announcement glue in
 * main/uart_sleep_window.c (uart_sleep_maybe_sleep_idf).
 *
 * The glue links against stubbed IDF calls, but the function under test is the
 * real production one. It pins the fix for the "receiver stuck on sleeping"
 * bug: the awake announcement (state=0) used to be gated on the CPU having
 * really parked (slept_ms > 0). When the park was skipped (another task held an
 * APB/PM lock, the scheduler was delayed, ...) slept_ms stayed 0, no state=0 was
 * ever sent, and the receiver was left believing the sender was asleep forever.
 * The announcement pair must now be symmetric: whenever state=1 was announced,
 * state=0 must follow.
 */
#include <stdint.h>
#include <stdio.h>

#include "uart_sleep_window.h"

/* Use the host shim's own types so the stub signatures match exactly. */
#include "esp_pm.h"
#include "driver/uart.h"
#include "esp_now.h"
#include "esp_wifi.h"

/* ---- stubs for the IDF surface uart_sleep_window.c links against ---------- */
int64_t esp_timer_get_time(void)
{
    static int64_t t;
    return (t += 1000);
}

esp_err_t esp_pm_lock_create(int type, int arg, const char *name,
                             esp_pm_lock_handle_t *out)
{
    (void)type;
    (void)arg;
    (void)name;
    if (out != NULL) {
        *out = (esp_pm_lock_handle_t)1;
    }
    return ESP_OK;
}
void esp_pm_lock_acquire(esp_pm_lock_handle_t h) { (void)h; }
void esp_pm_lock_release(esp_pm_lock_handle_t h) { (void)h; }
esp_err_t esp_pm_configure(const esp_pm_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size)
{
    (void)uart_num;
    if (size != NULL) {
        *size = 0; /* after the window the buffer is empty -> "clean" sleep */
    }
    return ESP_OK;
}
esp_err_t esp_now_set_wake_window(uint16_t window) { (void)window; return ESP_OK; }
esp_err_t esp_wifi_connectionless_module_set_wake_interval(uint16_t interval)
{
    (void)interval;
    return ESP_OK;
}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type) { (void)type; return ESP_OK; }

/* ---- assertions ---------------------------------------------------------- */
static int g_checks;
static int g_failures;
#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

/* Records the state announcements the glue emits. */
static int g_announce_count;
static bool g_announce_states[8];

static void record_announce(bool sleeping)
{
    if (g_announce_count < (int)(sizeof(g_announce_states) / sizeof(g_announce_states[0]))) {
        g_announce_states[g_announce_count] = sleeping;
    }
    ++g_announce_count;
}

/* Make the controller eligible to sleep: feed one byte (burst), then a 200 ms
 * quiet gap the parser is waiting through, then mark the burst idle. */
static void arm_sleep_eligible(void)
{
    uart_sleep_note_uart_byte(1000000, 0, 1, true);       /* first byte: burst */
    uart_sleep_note_uart_byte(1200000, 1000000, 1, true); /* 200 ms quiet gap */
    uart_sleep_note_uart_idle();
}

int main(void)
{
    CHECK(uart_sleep_init_idf(), "the sleep glue initialises");
    uart_sleep_set_state_notify(record_announce);

    /* 1) Announcement pair is symmetric. On the host the stubbed vTaskDelay()
     * cannot advance the private sleep counter, so slept_ms is always 0 here -
     * which is exactly the "park skipped" case the fix targets. The pair must
     * still be complete: state=1 then state=0. */
    arm_sleep_eligible();
    g_announce_count = 0;
    uart_sleep_set_slept_ms(0);
    uart_sleep_maybe_sleep_idf();
    CHECK(g_announce_count == 2, "an announced sleep also announces 'awake'");
    CHECK(g_announce_states[0] == true, "first announcement is 'sleeping'");
    CHECK(g_announce_states[1] == false, "second announcement is 'awake'");

    /* 2) Regression, the exact bug: slept_ms stays 0 (park skipped because
     * another task held an APB/PM lock, or the scheduler was delayed). The
     * awake announcement must STILL be sent - otherwise the receiver stays on
     * "sleeping" forever. Old code gated state=0 on slept_ms > 0 and failed
     * here. */
    arm_sleep_eligible();
    g_announce_count = 0;
    uart_sleep_set_slept_ms(500); /* unchanged across the call -> slept_ms == 0 */
    uart_sleep_maybe_sleep_idf();
    CHECK(g_announce_count == 2,
          "a skipped park still announces twice (no stuck 'sleeping')");
    CHECK(g_announce_states[0] == true, "skipped park still announces 'sleeping'");
    CHECK(g_announce_states[1] == false, "skipped park still announces 'awake'");

    /* 3) No sleep requested (bytes flowing): no announcement at all, so the
     * receiver is never told about a transition that did not happen. A 30 ms
     * gap is below the 50 ms threshold, so the controller is inside a burst. */
    uart_sleep_note_uart_byte(1230000, 1200000, 1, true); /* 30 ms -> burst */
    g_announce_count = 0;
    uart_sleep_maybe_sleep_idf();
    CHECK(g_announce_count == 0, "no announcement while a burst is in progress");

    /* 4) A null hook must not crash. */
    uart_sleep_set_state_notify(NULL);
    arm_sleep_eligible();
    uart_sleep_set_slept_ms(1000);
    uart_sleep_maybe_sleep_idf();
    CHECK(true, "a null state-notify hook is tolerated");

    printf("sleep announce: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
