/* Host tests for the boot-log ring and its suppress list.
 *
 * The receiver's /bootlog page is only useful if the startup messages survive
 * the ~6 kB ring. Per-telegram chatter ("SML value: ...") otherwise overwrites
 * them within seconds, so the suppress list must keep those lines out while
 * still capturing everything else. This drives the real boot_log.c. */
#include <stdio.h>
#include <string.h>

#include "boot_log.h"

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

static void capture(const char *s)
{
    boot_log_capture_line(s, strlen(s));
}

static void reset_ring(void)
{
    /* There is no reset API; read with a 1-byte buffer is useless for clearing.
     * Instead rely on the ring being overwritten by a large filler, which is
     * what the real firmware experiences anyway. */
    char filler[128];
    memset(filler, 'x', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\n';
    for (int i = 0; i < 200; ++i) {
        boot_log_capture_line(filler, sizeof(filler));
    }
}

int main(void)
{
    /* Install the ring (mutex + hook). On the host the hook is a no-op stub,
     * but boot_log_capture_line() writes to the same ring the hook uses, so the
     * filtering path is exercised for real. */
    boot_log_init();
    boot_log_suppress("SML value:");

    reset_ring();

    capture("boot: receiver starting\n");
    capture("SML value: OBIS=1.0.16.7.0 value=1234 scaler=-1 unit=27\n");
    capture("SML value: OBIS=1.0.1.8.0 value=5678 scaler=-1 unit=30\n");
    capture("wifi: connected on channel 1\n");

    char out[8192];
    size_t n = boot_log_read(out, sizeof(out));

    CHECK(strstr(out, "boot: receiver starting") != NULL,
          "startup line must be captured");
    CHECK(strstr(out, "wifi: connected on channel 1") != NULL,
          "later startup line must be captured");
    CHECK(strstr(out, "SML value:") == NULL,
          "suppressed SML lines must not reach the ring");
    CHECK(n > 0, "ring must contain data");

    /* A line that merely mentions SML but is not the per-value chatter must
     * still be captured: suppression is substring-based and precise. */
    capture("SML batch ring full; oldest telegram evicted\n");
    n = boot_log_read(out, sizeof(out));
    CHECK(strstr(out, "SML batch ring full") != NULL,
          "unrelated SML error lines must still be captured");

    /* A marker registered with an empty string is ignored. */
    boot_log_suppress("");
    capture("after empty marker\n");
    n = boot_log_read(out, sizeof(out));
    CHECK(strstr(out, "after empty marker") != NULL,
          "empty suppress marker must be a no-op");

    printf("boot_log: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
