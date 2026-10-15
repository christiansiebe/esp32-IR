/* Host-test shim: enough of driver/uart.h to syntax-check the sleep-window glue. */
#pragma once
#include "esp_shim.h"

typedef int uart_port_t;

#define UART_NUM_0 ((uart_port_t)0)
#define UART_NUM_1 ((uart_port_t)1)

/* Host stub of the legacy UART clock-source enum (esp32/clk_tree_defs.h). Only
 * the two values power_tuning.h selects between are needed. */
typedef enum {
    UART_SCLK_APB = 1,
    UART_SCLK_REF_TICK = 2,
    UART_SCLK_DEFAULT = UART_SCLK_APB,
} uart_sclk_t;

esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size);
