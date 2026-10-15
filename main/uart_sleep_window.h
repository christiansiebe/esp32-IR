#pragma once

/* Sender-side adaptive light-sleep window.
 *
 * The SML UART is an external device that sends when *it* wants, so a fixed
 * sleep window either wastes power (too small) or clips the head of the next
 * telegram while the APB clock is off (too large -> bytes are lost). This
 * controller adapts the window from what the UART actually does:
 *
 *   - It tracks the *shortest* quiet interval seen between telegrams.
 *   - The window is at most (shortest quiet * (1 - margin)).
 *   - If a telegram ever arrived while the CPU was asleep (a "short sleep"),
 *     the window is cut hard; it then grows slowly while sleeps stay clean.
 *
 * Pure C, no IDF: the state machine is host-tested (test_uart_sleep_window.c).
 * main.c supplies timestamps and performs the actual light-sleep request.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t initial_ms;    /* first window before any telegram is seen */
    uint32_t min_ms;        /* never sleep less than this */
    uint32_t max_ms;        /* never sleep more than this */
    uint32_t grow_ms;       /* additive growth per clean sleep */
    uint32_t backoff_num;   /* multiply window by num/den when a sleep was too long */
    uint32_t backoff_den;
    uint32_t margin_num;    /* cap window at last_min_gap * num/den */
    uint32_t margin_den;
} uart_sleep_cfg_t;

typedef struct {
    uart_sleep_cfg_t cfg;
    uint32_t window_ms;     /* current requested window */
    bool have_burst;        /* a burst is in progress (bytes seen recently) */
    uint32_t last_min_gap_ms; /* shortest inter-telegram quiet seen (0 = none) */
} uart_sleep_ctrl_t;

/* Sensible defaults for the 9600-baud SML stream (~700-850 ms quiet between
 * telegrams on the bench): 400 ms initial, never below 100 ms or above 1500 ms. */
static inline uart_sleep_cfg_t uart_sleep_default_cfg(void)
{
    uart_sleep_cfg_t c;
    c.initial_ms = 400;
    c.min_ms = 100;
    c.max_ms = 1500;
    c.grow_ms = 25;
    c.backoff_num = 1;
    c.backoff_den = 2;
    c.margin_num = 3;
    c.margin_den = 4;
    return c;
}

static inline void uart_sleep_init(uart_sleep_ctrl_t *s, uart_sleep_cfg_t cfg)
{
    s->cfg = cfg;
    s->window_ms = cfg.initial_ms;
    s->have_burst = false;
    s->last_min_gap_ms = 0;
}

/* Recompute window from the shortest quiet interval: window <= min_gap*margin,
 * clamped to [min_ms, max_ms]. */
static inline void uart_sleep_apply_cap(uart_sleep_ctrl_t *s)
{
    if (s->last_min_gap_ms != 0) {
        uint32_t cap = (uint32_t)(((uint64_t)s->last_min_gap_ms * s->cfg.margin_num) /
                                  s->cfg.margin_den);
        if (s->window_ms > cap) {
            s->window_ms = cap;
        }
    }
    if (s->window_ms < s->cfg.min_ms) {
        s->window_ms = s->cfg.min_ms;
    }
    if (s->window_ms > s->cfg.max_ms) {
        s->window_ms = s->cfg.max_ms;
    }
}

/* Feed every received UART byte. A quiet stretch at/after gap_threshold_ms
 * between bytes is a candidate quiet interval, but it only tightens the window
 * when `awaiting` says the parser is genuinely between telegrams: the meter also
 * pauses ~50-90 ms *inside* a telegram, and using those would pin the window to
 * min_ms and make it sleep mid-telegram. */
static inline void uart_sleep_note_byte(uart_sleep_ctrl_t *s, uint64_t now_us,
                                        uint64_t last_byte_us,
                                        uint32_t gap_threshold_ms, bool awaiting)
{
    (void)now_us;
    uint64_t gap_us = (last_byte_us != 0) ? (now_us - last_byte_us) : 0;
    uint32_t gap_ms = (uint32_t)(gap_us / 1000ULL);
    if (gap_ms >= gap_threshold_ms) {
        if (awaiting && (s->last_min_gap_ms == 0 || gap_ms < s->last_min_gap_ms)) {
            s->last_min_gap_ms = gap_ms;
        }
    } else {
        /* Bytes are flowing: a burst is in progress, do not sleep. */
        s->have_burst = true;
    }
    uart_sleep_apply_cap(s);
}

/* Tell the controller the burst ended (UART read timeout, i.e. the line has
 * been quiet for at least the fixed gap threshold). */
static inline void uart_sleep_burst_idle(uart_sleep_ctrl_t *s)
{
    s->have_burst = false;
}

static inline bool uart_sleep_should_request(const uart_sleep_ctrl_t *s)
{
    /* Only sleep in the quiet gap between telegram bursts; never while bytes
     * are still arriving, and never before any telegram has been seen. */
    return !s->have_burst && s->last_min_gap_ms != 0 && s->window_ms != 0;
}

static inline uint32_t uart_sleep_window_us(const uart_sleep_ctrl_t *s)
{
    return s->window_ms * 1000u;
}

/* The CPU slept cleanly for the full window: let the window grow slowly.
 * Only called when the CPU actually slept (slept_ms != 0): a window skipped
 * because a hold or a full UART buffer kept the CPU awake must NOT count as a
 * clean sleep, or the controller grows a window it never tested. */
static inline void uart_sleep_note_clean(uart_sleep_ctrl_t *s, uint32_t slept_ms)
{
    if (slept_ms == 0) {
        return;
    }
    uint32_t grown = s->window_ms + s->cfg.grow_ms;
    if (grown > s->cfg.max_ms) {
        grown = s->cfg.max_ms;
    }
    s->window_ms = grown;
    uart_sleep_apply_cap(s);
}

/* A telegram arrived mid-sleep (bytes were likely lost): cut the window hard.
 * This is the "auto-adjust if 400 ms is not enough" behaviour. */
static inline void uart_sleep_note_short_sleep(uart_sleep_ctrl_t *s)
{
    uint32_t next = (uint32_t)(((uint64_t)s->window_ms * s->cfg.backoff_num) /
                               s->cfg.backoff_den);
    if (next < s->cfg.min_ms) {
        next = s->cfg.min_ms;
    }
    s->window_ms = next;
    uart_sleep_apply_cap(s);
}

/* --- IDF glue implemented in uart_sleep_window.c ---
 * Declared unconditionally so the sender call sites need no #if; the functions
 * are no-ops when CONFIG_ESPNOW_SENDER_LIGHT_SLEEP is off. */
bool uart_sleep_init_idf(void);
void uart_sleep_note_uart_byte(uint64_t now_us, uint64_t last_byte_us, uint32_t received,
                               bool awaiting);
void uart_sleep_note_uart_idle(void);
void uart_sleep_maybe_sleep_idf(void);

/* Keep the CPU awake for at least `hold_ms`. Called from the ESP-NOW receive
 * path so an in-flight history pull or OTA keeps the sender responsive: every
 * packet refreshes the hold, and it self-clears once traffic stops (no
 * acquire/release pairing to leak). */
void uart_sleep_hold(uint32_t hold_ms);

/* main.c feeds the accumulated light-sleep time (its sleep-exit callback owns
 * the counter) so the controller only grows a window it actually slept. */
void uart_sleep_set_slept_ms(uint32_t ms);

/* True while a hold set with uart_sleep_hold() is still active. Pure so it is
 * host-tested. */
static inline bool uart_sleep_hold_active(int64_t hold_until_us, int64_t now_us)
{
    return hold_until_us != 0 && now_us < hold_until_us;
}

/* Diagnostics for the on-demand sender counters page (no-ops when disabled). */
uint32_t uart_sleep_window_ms(void);
uint32_t uart_sleep_min_gap_ms(void);
bool uart_sleep_hold_is_active(void);
