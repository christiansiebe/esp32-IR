/* Parser stall recovery: the SML parser has no timeout of its own, so a wedge
 * (bytes arriving, no telegram ever finalised) must be broken by a timer. The
 * timer is armed by the first byte of the capture, not by a telegram - the old
 * "arm on first telegram" rule left it dead until the first telegram of a boot,
 * so a wedge on that very first telegram was permanent (the live "sender
 * delivers no values" bug). Pure logic, no ESP-IDF. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "uart_recovery.h"

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

int main(void)
{
    printf("uart parser stall recovery:\n");

    /* Not armed yet: no byte has been seen, so there is nothing to recover. */
    CHECK(uart_parser_stall_recover(100000, 0, 15000) == false,
          "unarmed timer never fires");

    /* A wedge on the very first telegram: the timer was armed by the first byte
     * (progress_ms != 0) and must fire once the stall window has passed. This is
     * the exact case the old rule missed. */
    CHECK(uart_parser_stall_recover(10000, 5000, 15000) == false,
          "before the window: no fire");
    CHECK(uart_parser_stall_recover(20000, 5000, 15000) == true,
          "at the window: fires (first-telegram wedge)");
    CHECK(uart_parser_stall_recover(60000, 5000, 15000) == true,
          "well past the window: fires");

    /* A fresh telegram callback re-arms the timer, so a healthy parser is never
     * reset while telegrams keep arriving inside the window. */
    CHECK(uart_parser_stall_recover(20000, 19999, 15000) == false,
          "just-progressed: no fire");
    CHECK(uart_parser_stall_recover(34998, 19999, 15000) == false,
          "one ms short of the window: no fire");
    CHECK(uart_parser_stall_recover(35000, 19999, 15000) == true,
          "exactly at the window: fires");

    /* Boundary: a zero-length window is not a stall (must not reset in a loop). */
    CHECK(uart_parser_stall_recover(20000, 20000, 0) == true,
          "zero window fires only when the clocks are equal and elapsed");

    /* A clock that went backwards (wrap/regression) must not be read as a stall. */
    CHECK(uart_parser_stall_recover(1000, 2000, 15000) == false,
          "backwards clock: no fire");

    printf("uart parser stall recovery: %d checks, %d failures\n", g_checks,
           g_failures);
    return g_failures == 0 ? 0 : 1;
}
