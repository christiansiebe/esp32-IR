/* Host-test shim: enough of driver/uart.h to syntax-check the sleep-window glue. */
#pragma once
#include "esp_shim.h"

typedef int uart_port_t;

#define UART_NUM_0 ((uart_port_t)0)
#define UART_NUM_1 ((uart_port_t)1)

esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size);
esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size,
                              int tx_buffer_size, int queue_size, void *queue,
                              int flags);
int uart_read_bytes(uart_port_t uart_num, uint8_t *buf, uint32_t length,
                    uint32_t ticks_to_wait);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
