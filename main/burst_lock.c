#include "burst_lock.h"

#include <stddef.h>

#define BURST_LOCK_DEFAULT_GAP_MS 1000u
#define BURST_LOCK_DEFAULT_WINDOW_MS 400u
#define BURST_LOCK_DEFAULT_MAX_MISSES 3u
#define BURST_LOCK_MIN_PERIOD_MS 1000u
#define BURST_LOCK_MAX_PERIOD_MS 20000u
/* A window stays open (past window_ms) only while frames keep landing within
 * this quiet gap; once the burst goes quiet for this long, it closes. Must stay
 * above the sender's retransmit cadence (SML_ACK_TIMEOUT_MS +
 * SML_ACK_RETRY_BACKOFF_MS), or a retry would land after the radio parked. */
#define BURST_LOCK_EXTEND_QUIET_MS 150u

void burst_lock_init(burst_lock_t *bl, uint32_t gap_ms, uint32_t window_ms,
                     uint32_t lead_ms, uint32_t max_misses)
{
    if (bl == NULL) {
        return;
    }
    *bl = (burst_lock_t){
        .gap_ms = gap_ms != 0u ? gap_ms : BURST_LOCK_DEFAULT_GAP_MS,
        .window_ms = window_ms != 0u ? window_ms : BURST_LOCK_DEFAULT_WINDOW_MS,
        .max_misses = max_misses != 0u ? max_misses : BURST_LOCK_DEFAULT_MAX_MISSES,
        .min_period_ms = BURST_LOCK_MIN_PERIOD_MS,
        .max_period_ms = BURST_LOCK_MAX_PERIOD_MS,
    };
    /* The lead must leave room for a usable window. */
    bl->lead_ms = lead_ms < bl->window_ms ? lead_ms : bl->window_ms - 1u;
}

bool burst_lock_note_frame(burst_lock_t *bl, uint32_t now_ms)
{
    if (bl == NULL) {
        return false;
    }
    /* Unsigned subtraction stays correct across the 32-bit ms rollover
     * (~49.7 days), so a frame after the wrap is not mistaken for a burst. */
    bool new_burst = !bl->have_burst ||
                     (uint32_t)(now_ms - bl->last_frame_ms) >= bl->gap_ms;
    bl->last_frame_ms = now_ms;

    if (!new_burst) {
        /* A later frame of a burst the window is already watching: nothing to
         * re-anchor, but it does prove the window was served. */
        if (bl->in_window) {
            bl->window_served = true;
        }
        return false;
    }

    bl->have_burst = true;
    ++bl->bursts;
    if (bl->bursts > 1u) {
        uint32_t delta = (uint32_t)(now_ms - bl->last_burst_ms);
        if (delta >= bl->min_period_ms && delta <= bl->max_period_ms) {
            bl->period_ms = delta;
            if (!bl->locked) {
                bl->locked = true;
                bl->misses = 0u;
                ++bl->locks;
            }
        }
    }
    bl->last_burst_ms = now_ms;
    if (bl->in_window) {
        bl->window_served = true;
    }
    return true;
}

bool burst_lock_locked(const burst_lock_t *bl)
{
    return bl != NULL && bl->locked;
}

int32_t burst_lock_wait_ms(const burst_lock_t *bl, uint32_t now_ms)
{
    if (bl == NULL || !bl->locked || bl->period_ms == 0u) {
        return 0;
    }
    /* Open the window `lead_ms` before the predicted burst, one period after the
     * last one we caught. */
    uint32_t target = bl->last_burst_ms + bl->period_ms - bl->lead_ms;
    int32_t remaining = (int32_t)(target - now_ms);
    return remaining > 0 ? remaining : 0;
}

void burst_lock_window_open(burst_lock_t *bl)
{
    if (bl == NULL) {
        return;
    }
    bl->in_window = true;
    bl->window_served = false;
}

bool burst_lock_window_should_extend(const burst_lock_t *bl, uint32_t open_ms,
                                     uint32_t now_ms, uint32_t max_open_ms)
{
    if (bl == NULL || !bl->in_window) {
        return false;
    }
    /* Hard cap: never hold the radio awake past this. */
    if ((uint32_t)(now_ms - open_ms) >= max_open_ms) {
        return false;
    }
    /* Keep open only while frames are still landing; a quiet gap ends it. */
    return (uint32_t)(now_ms - bl->last_frame_ms) < BURST_LOCK_EXTEND_QUIET_MS;
}

void burst_lock_window_close(burst_lock_t *bl)
{
    if (bl == NULL) {
        return;
    }
    bl->in_window = false;
    if (bl->window_served) {
        bl->misses = 0u;
        ++bl->window_hits;
        return;
    }
    ++bl->window_misses;
    if (bl->max_misses != 0u && ++bl->misses >= bl->max_misses) {
        /* Several empty windows in a row: the sender's phase moved (reboot) or
         * the meter went quiet. Drop the lock so the caller stays awake and
         * re-learns instead of parking through real traffic. */
        bl->locked = false;
        bl->period_ms = 0u;
        bl->have_burst = false;
        bl->misses = 0u;
        ++bl->unlocks;
    }
}
