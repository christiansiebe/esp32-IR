#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Parser stall recovery decision.
 *
 * The SML parser is a byte state machine with no timeout of its own. If it
 * desynchronises it can sit in an intermediate state indefinitely, consuming
 * bytes without ever reaching SML_FINAL or SML_CHECKSUM_ERROR, so no telegram
 * callback fires. That is exactly the "sender delivers no values" failure:
 * bytes keep arriving (uart_rx_bytes climbs, no FIFO/frame/parity errors) but
 * nothing is ever sent.
 *
 * The recovery timer must be armed by *bytes*, not by a telegram. Gating it on
 * "a telegram was finalised before" (the previous behaviour) left it dead until
 * the first telegram of a boot, so a wedge on that very first telegram was
 * permanent - there was never a prior telegram to arm the timer.
 *
 * Pure C, no ESP-IDF, so the timing rule is host-tested
 * (test/test_uart_recovery.c). */

/* progress_ms: the last time the parser made observable progress. Either a
 * telegram callback fired (CRC valid *or* checksum error), or - before any
 * telegram - the first byte of this capture arrived, which arms the timer.
 * 0 means "not armed yet". */
bool uart_parser_stall_recover(uint64_t now_ms, uint64_t progress_ms,
                               uint32_t stall_ms);
