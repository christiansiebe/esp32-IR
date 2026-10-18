#include "uart_recovery.h"

bool uart_parser_stall_recover(uint64_t now_ms, uint64_t progress_ms,
                               uint32_t stall_ms)
{
    if (progress_ms == 0) {
        return false; /* no byte seen yet; nothing to recover from */
    }
    if (now_ms < progress_ms) {
        return false; /* clock went backwards; wait for it to catch up */
    }
    return (now_ms - progress_ms) >= (uint64_t)stall_ms;
}
