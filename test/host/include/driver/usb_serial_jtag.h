/* Host-test shim: enough of driver/usb_serial_jtag.h to syntax-check the
 * ESP32-C6 serial-console glue (main/serial_console.c, USB branch). The
 * functions are not defined here; only the -fsyntax-only CI check compiles
 * against this header, so no link is needed. */
#pragma once
#include "esp_shim.h"

typedef struct {
    size_t tx_buffer_size;
    size_t rx_buffer_size;
} usb_serial_jtag_driver_config_t;

#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() \
    ((usb_serial_jtag_driver_config_t){.tx_buffer_size = 256, .rx_buffer_size = 256})

esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config);
int usb_serial_jtag_write_bytes(const void *src, size_t size, uint32_t ticks_to_wait);
int usb_serial_jtag_read_bytes(void *buf, uint32_t size, uint32_t ticks_to_wait);
