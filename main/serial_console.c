#include "serial_console.h"

#include <string.h>

#include "web_console.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The IDF console (and the ROM bootloader) use UART0; CONFIG_ESP_CONSOLE_UART_NUM
 * defaults to it. The SML read head is on UART1, so this never touches it. */
#define SERIAL_CONSOLE_UART UART_NUM_0
#define SERIAL_CONSOLE_RX_BUF 1024
#define SERIAL_CONSOLE_LINE_MAX 192
#define SERIAL_CONSOLE_STACK 4096
#define SERIAL_CONSOLE_PRIO 3

static const char *TAG = "serial_console";

static void serial_console_task(void *arg)
{
    (void)arg;
    /* The UART0 driver is installed for the log console too; installing it again
     * would return ESP_ERR_INVALID_STATE, which just means the RX path is ready
     * to read from. */
    esp_err_t err = uart_driver_install(SERIAL_CONSOLE_UART, SERIAL_CONSOLE_RX_BUF,
                                        0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "serial console unavailable: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Serial console ready - type 'help' (set freq 80, test sleep 400)");

    char line[SERIAL_CONSOLE_LINE_MAX];
    size_t used = 0;
    while (1) {
        uint8_t ch;
        int got = uart_read_bytes(SERIAL_CONSOLE_UART, &ch, 1,
                                  pdMS_TO_TICKS(100));
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
            uart_write_bytes(SERIAL_CONSOLE_UART, "\r\n", 2);
            uart_write_bytes(SERIAL_CONSOLE_UART, out, strlen(out));
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
