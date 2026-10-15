/* Host tests for the pure power/robustness helpers in main/power_tuning.h.
 *
 * These lock in the two decisions that are easy to get wrong and that matter on
 * the bench:
 *   - the UART RX clock must be REF_TICK whenever DFS can change the APB clock,
 *     otherwise the baud-rate divider goes stale and the checksum fails;
 *   - the ESP-NOW wake window must never be >= the wake interval (that would
 *     mean "listen continuously"), and the beacon interval must satisfy the
 *     Wi-Fi API's range/multiple-of-100 contract. */

#include <stdio.h>
#include <stdlib.h>

#include "power_tuning.h"

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static void test_uart_rx_clock(void)
{
    /* The core guarantee: with PM on (DFS possible) the RX clock is REF_TICK. */
    CHECK(uart_pick_rx_source_clk(true) == UART_SCLK_REF_TICK);
    /* With PM off the APB clock is fixed, so keep the API default. */
    CHECK(uart_pick_rx_source_clk(false) == UART_SCLK_DEFAULT);
    CHECK(uart_pick_rx_source_clk(false) == UART_SCLK_APB);
    /* The compiled-in decision must match the helper. */
    CHECK(UART_RX_PM_ENABLED ==
          (uart_pick_rx_source_clk(UART_RX_PM_ENABLED) == UART_SCLK_REF_TICK));
}

static void test_espnow_ps_clamp(void)
{
    uint16_t interval = 0;
    uint16_t window = 0;

    /* Normal 70% case passes through unchanged. */
    espnow_ps_clamp(100, 70, &interval, &window);
    CHECK(interval == 100 && window == 70);

    /* window >= interval would mean "listen continuously": clamp to interval-1. */
    espnow_ps_clamp(100, 100, &interval, &window);
    CHECK(interval == 100 && window == 99);
    espnow_ps_clamp(100, 5000, &interval, &window);
    CHECK(interval == 100 && window == 99);

    /* interval 0 is out of the API range (1..65535): clamp to 1, window to 0. */
    espnow_ps_clamp(0, 0, &interval, &window);
    CHECK(interval == 1 && window == 0);
    espnow_ps_clamp(0, 50, &interval, &window);
    CHECK(interval == 1 && window == 0);

    /* Boundary: interval 1 -> window 0. */
    espnow_ps_clamp(1, 1, &interval, &window);
    CHECK(interval == 1 && window == 0);

    /* Large but in-range values are not truncated by the clamp. */
    espnow_ps_clamp(65535, 65535, &interval, &window);
    CHECK(interval == 65535 && window == 65534);
}

static void test_beacon_interval_clamp(void)
{
    /* The API default is preserved exactly. */
    CHECK(ap_beacon_interval_clamp(100) == 100);
    /* Below range -> 100. */
    CHECK(ap_beacon_interval_clamp(0) == 100);
    CHECK(ap_beacon_interval_clamp(50) == 100);
    /* Above range -> 60000. */
    CHECK(ap_beacon_interval_clamp(60001) == 60000);
    CHECK(ap_beacon_interval_clamp(100000) == 60000);
    /* Rounded down to a multiple of 100. */
    CHECK(ap_beacon_interval_clamp(250) == 200);
    CHECK(ap_beacon_interval_clamp(399) == 300);
    CHECK(ap_beacon_interval_clamp(300) == 300);
    /* Boundaries stay valid multiples of 100 within [100, 60000]. */
    CHECK(ap_beacon_interval_clamp(60000) == 60000);
    CHECK(ap_beacon_interval_clamp(59999) == 59900);
}

int main(void)
{
    test_uart_rx_clock();
    test_espnow_ps_clamp();
    test_beacon_interval_clamp();

    if (failures != 0) {
        fprintf(stderr, "test_power_tuning: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_power_tuning: all checks passed\n");
    return 0;
}
