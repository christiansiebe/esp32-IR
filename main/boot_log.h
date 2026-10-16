/* Boot log capture in RAM, readable over Wi-Fi.
 *
 * The receiver's only output path is UART0. If no host is attached at boot,
 * the boot log is lost. This module keeps the last BOOT_LOG_CAPACITY bytes of
 * the log in a RAM ring by intercepting esp_log's output (esp_log_set_vprintf),
 * so the same text is still there after the network comes up and can be viewed
 * at /bootlog.
 *
 * Not a replacement for the serial monitor: nothing before this capture starts
 * (early startup, the first SPIFFS mount, etc.) is included, and it does not
 * survive a restart.
 */
#ifndef BOOT_LOG_H
#define BOOT_LOG_H

#include <stddef.h>

/* Install the log hook and start capturing. Call as early as possible in
 * app_main. Safe to call once; a second call is a no-op. */
void boot_log_init(void);

/* Copy up to max bytes of the captured log into out (NUL terminated).
 * Returns the number of bytes written. */
size_t boot_log_read(char *out, size_t max);

/* Total bytes captured since boot, including those already overwritten in the
 * ring. Useful to detect that the log was truncated. */
size_t boot_log_total(void);

#endif /* BOOT_LOG_H */
