/* Serial (UART0) command console.
 *
 * Mirrors the web debug console onto the USB serial monitor: lines typed in
 * `idf.py monitor` (or any 115200 8N1 terminal) are dispatched through the same
 * web_console command set, so `set freq 80`, `test sleep`, `status`, ... can be
 * driven without the Wi-Fi dashboard. Runs on the IDF console UART (UART0); the
 * SML read head is on UART1, so there is no conflict. */
#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

void serial_console_start(void);

#endif /* SERIAL_CONSOLE_H */
