#include "serial_console.h"

#include <string.h>

#include "web_console.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Where the serial console reads and writes. On the ESP32 classic this is UART0
 * (the same port the IDF log console and the ROM bootloader use; the SML read
 * head is on UART1, so this never touches it). The ESP32-C6 has no USB-UART
 * bridge: its console is the USB Serial/JTAG peripheral, which is what
 * /dev/ttyACM* enumerates as and what the C6 sdkconfig defaults select as the
 * primary console (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG). */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#define SERIAL_CONSOLE_USE_USB_JTAG 1
#include "driver/usb_serial_jtag.h"
#else
#define SERIAL_CONSOLE_USE_USB_JTAG 0
#include "driver/uart.h"
#define SERIAL_CONSOLE_UART UART_NUM_0
#endif

#define SERIAL_CONSOLE_RX_BUF 1024
/* The USB Serial/JTAG TX ring must hold a whole reply: xRingbufferSend() drops a
 * write that does not fit entirely, so the default 256 B silently swallowed the
 * output of every command longer than that (help, status). The UART path is
 * unaffected (it blocks until the bytes are out). */
#define SERIAL_CONSOLE_TX_BUF 1024
#define SERIAL_CONSOLE_TX_CHUNK 128
#define SERIAL_CONSOLE_LINE_MAX 192
#define SERIAL_CONSOLE_STACK 4096
#define SERIAL_CONSOLE_PRIO 3

static const char *TAG = "serial_console";

#if SERIAL_CONSOLE_USE_USB_JTAG
static void console_write(const char *data, size_t len)
{
    /* Send in chunks that always fit the TX ring: a single oversized write is
     * dropped by xRingbufferSend(), which is what made long replies empty. Each
     * blocking write drains as the ISR empties the ring. */
    while (len > 0) {
        size_t chunk = len > SERIAL_CONSOLE_TX_CHUNK ? SERIAL_CONSOLE_TX_CHUNK : len;
        int written = usb_serial_jtag_write_bytes(data, chunk, portMAX_DELAY);
        if (written <= 0) {
            break;
        }
        data += written;
        len -= (size_t)written;
    }
}

static int console_read_byte(uint8_t *ch)
{
    return usb_serial_jtag_read_bytes(ch, 1, pdMS_TO_TICKS(100));
}

static bool console_install(void)
{
    /* The USB Serial/JTAG driver is not installed by the log console (which
     * writes through the ROM directly), so this owns the RX ring. */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = SERIAL_CONSOLE_RX_BUF;
    cfg.tx_buffer_size = SERIAL_CONSOLE_TX_BUF;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}
#else
static void console_write(const char *data, size_t len)
{
    uart_write_bytes(SERIAL_CONSOLE_UART, data, len);
}

static int console_read_byte(uint8_t *ch)
{
    return uart_read_bytes(SERIAL_CONSOLE_UART, ch, 1, pdMS_TO_TICKS(100));
}

static bool console_install(void)
{
    /* The UART0 driver is installed for the log console too; installing it again
     * would return ESP_ERR_INVALID_STATE, which just means the RX path is ready
     * to read from. */
    esp_err_t err = uart_driver_install(SERIAL_CONSOLE_UART, SERIAL_CONSOLE_RX_BUF,
                                        0, 0, NULL, 0);
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}
#endif

static void serial_console_task(void *arg)
{
    (void)arg;
    if (!console_install()) {
        ESP_LOGW(TAG, "serial console unavailable");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Serial console ready - type 'help' (set freq 80, test sleep 400)");

    char line[SERIAL_CONSOLE_LINE_MAX];
    size_t used = 0;
    while (1) {
        uint8_t ch;
        int got = console_read_byte(&ch);
        if (got <= 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (used == 0) {
                continue; /* ignore CR of a CRLF pair / empty lines */
            }
            line[used] = '\0';
            used = 0;
            char out[768];
            web_console_run(line, out, sizeof(out));
            /* \r\n so a plain terminal shows the result on its own line. */
            console_write("\r\n", 2);
            console_write(out, strlen(out));
            continue;
        }
        if (ch == 0x7f || ch == 0x08) { /* backspace */
            if (used > 0) {
                --used;
            }
            continue;
        }
        if (used < sizeof(line) - 1) {
            line[used++] = (char)ch;
        }
    }
}

void serial_console_start(void)
{
    if (xTaskCreate(serial_console_task, "serial_console", SERIAL_CONSOLE_STACK,
                    NULL, SERIAL_CONSOLE_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the serial console");
    }
}
