/* Host-test shim: enough of driver/uart.h to syntax-check the sleep-window glue. */
#pragma once
#include "esp_shim.h"

typedef int uart_port_t;

#define UART_NUM_0 ((uart_port_t)0)
#define UART_NUM_1 ((uart_port_t)1)

esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size);
