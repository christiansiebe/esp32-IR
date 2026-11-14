/* RTC/LP watchdog (RWDT) backstop policy - the pure decision logic.
 *
 * Why a second, hardware watchdog: the task watchdog (TWDT) and the liveness
 * guard both need *code* to run to escalate, and their escalation ends in
 * esp_restart(). Neither helps when a SPIFFS/flash operation stalls the CPU
 * with the instruction cache disabled: no task runs, so nothing feeds and
 * nothing reboots. The RWDT is the only watchdog on the C6 that is clocked by
 * the RTC slow clock and resets the chip in hardware, independent of the CPU
 * and the flash cache.
 *
 * This module holds only the tick arithmetic (pure C, host-tested). The IDF
 * glue lives in rwdt_guard.c.
 */
#ifndef RWDT_POLICY_H
#define RWDT_POLICY_H

#include <stdint.h>

/* Default backstop timeout. Deliberately ABOVE the liveness escalation
 * (60 s warn + 30 s panic + 30 s reboot = 120 s): when the staged software
 * reboot still works it wins, and the hardware reset only catches the case
 * where even esp_restart() no longer runs. */
#define RWDT_DEFAULT_TIMEOUT_MS 180000u
#define RWDT_MIN_TIMEOUT_MS 10000u
#define RWDT_MAX_TIMEOUT_MS 3600000u

/* Clamp a requested timeout into the sane range above. */
uint32_t rwdt_clamp_timeout_ms(uint32_t timeout_ms);

/* Convert a millisecond timeout into RWDT stage-0 tick counts.
 *
 * RWDT stage 0 applies an implicit multiplier of 2^(1 + EFUSE_WDT_DELAY_SEL)
 * to the configured register value, so the low bits are dropped by the
 * hardware. Round UP to the next multiple of 16 (the worst-case multiplier,
 * sel = 3) so the effective timeout is never shorter than requested. A zero
 * timeout or an unknown (0 Hz) slow clock yields 0, which the caller must
 * treat as "do not arm". */
uint32_t rwdt_timeout_ticks(uint32_t timeout_ms, uint32_t slow_clk_hz);

#endif /* RWDT_POLICY_H */
