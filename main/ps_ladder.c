#include "ps_ladder.h"

#include <stddef.h>

static ps_ladder_mode_t ps_ladder_clamp(const ps_ladder_t *l,
                                        ps_ladder_mode_t mode)
{
    if (mode > PS_LADDER_MAX) {
        mode = PS_LADDER_MAX;
    }
    if (mode < l->floor_mode) {
        mode = l->floor_mode;
    }
    return mode;
}

void ps_ladder_init(ps_ladder_t *l, uint32_t window_ms, uint32_t loss_pct,
                    uint32_t min_frames, uint32_t min_losses_to_step,
                    uint32_t recovery_windows, ps_ladder_mode_t floor_mode,
                    ps_ladder_mode_t start_mode, uint32_t started_at_ms)
{
    if (l == NULL) {
        return;
    }
    l->window_ms = window_ms != 0u ? window_ms : PS_LADDER_DEFAULT_WINDOW_MS;
    l->loss_pct = loss_pct != 0u ? loss_pct : PS_LADDER_DEFAULT_LOSS_PCT;
    l->min_frames = min_frames;
    l->min_losses_to_step = min_losses_to_step != 0u ? min_losses_to_step
                                                      : PS_LADDER_DEFAULT_MIN_LOSSES;
    l->recovery_windows = recovery_windows != 0u ? recovery_windows
                                                 : PS_LADDER_DEFAULT_RECOVERY_WINDOWS;
    l->floor_mode = (floor_mode > PS_LADDER_MAX) ? PS_LADDER_MAX : floor_mode;
    l->start_mode = (start_mode > PS_LADDER_MAX) ? PS_LADDER_MAX : start_mode;
    if (l->start_mode < l->floor_mode) {
        l->start_mode = l->floor_mode;
    }
    l->window_start_ms = started_at_ms;
    l->losses = 0u;
    l->frames = 0u;
    l->clean_streak = 0u;
    l->step_failures[0] = 0u;
    l->step_failures[1] = 0u;
    l->step_failures[2] = 0u;
    l->mode = l->start_mode;
    l->enabled = true;
    l->total_losses = 0u;
    l->steps_down = 0u;
    l->steps_up = 0u;
}

void ps_ladder_note_loss(ps_ladder_t *l, uint32_t count)
{
    if (l == NULL || count == 0u) {
        return;
    }
    l->losses += count;
    l->total_losses += count;
}

void ps_ladder_note_frames(ps_ladder_t *l, uint32_t count)
{
    if (l != NULL && count != 0u) {
        l->frames += count;
    }
}

ps_ladder_mode_t ps_ladder_poll(ps_ladder_t *l, uint32_t now_ms)
{
    if (l == NULL) {
        return PS_LADDER_MAX;
    }
    if (!l->enabled) {
        return l->mode;
    }
    if ((uint32_t)(now_ms - l->window_start_ms) < l->window_ms) {
        return l->mode;
    }

    /* Loss ratio needs a real denominator: a quiet window (few frames) must not
     * be judged, and a single stray loss must not move a rung. */
    bool too_lossy = false;
    if (l->frames >= l->min_frames && l->losses >= l->min_losses_to_step) {
        too_lossy = (l->losses * 100u) >= (l->frames * l->loss_pct);
    }

    if (too_lossy) {
        if (l->mode > l->floor_mode) {
            /* Record that this rung cost us data before leaving it, so a later
             * climb will not blindly retry a rung that keeps failing. */
            if (l->mode <= PS_LADDER_MAX && l->step_failures[l->mode] < 255u) {
                ++l->step_failures[l->mode];
            }
            l->mode = (ps_ladder_mode_t)(l->mode - 1);
            ++l->steps_down;
        }
        l->clean_streak = 0u;
    } else if (l->losses == 0u && l->frames > 0u) {
        /* A clean, active window: after enough of them in a row, climb one rung
         * back (never above the configured start, and never back onto a rung
         * that already proved lossy twice - that would oscillate). */
        ++l->clean_streak;
        ps_ladder_mode_t up = (ps_ladder_mode_t)(l->mode + 1);
        bool up_confirmed_bad = (up <= PS_LADDER_MAX &&
                                 l->step_failures[up] >= PS_LADDER_MAX_RETRIES);
        if (l->clean_streak >= l->recovery_windows && l->mode < l->start_mode &&
            !up_confirmed_bad) {
            l->mode = up;
            ++l->steps_up;
            l->clean_streak = 0u;
        }
    } else {
        /* Some loss but not enough to step: not clean, so no climb. */
        l->clean_streak = 0u;
    }

    l->window_start_ms = now_ms;
    l->losses = 0u;
    l->frames = 0u;
    return l->mode;
}

ps_ladder_mode_t ps_ladder_mode(const ps_ladder_t *l)
{
    return l != NULL ? l->mode : PS_LADDER_MAX;
}

void ps_ladder_set_enabled(ps_ladder_t *l, bool on)
{
    if (l != NULL) {
        l->enabled = on;
    }
}

bool ps_ladder_enabled(const ps_ladder_t *l)
{
    return l != NULL && l->enabled;
}

void ps_ladder_force_mode(ps_ladder_t *l, ps_ladder_mode_t mode, uint32_t now_ms)
{
    if (l == NULL) {
        return;
    }
    l->mode = ps_ladder_clamp(l, mode);
    l->window_start_ms = now_ms;
    l->losses = 0u;
    l->frames = 0u;
    l->clean_streak = 0u;
}

void ps_ladder_set_start(ps_ladder_t *l, ps_ladder_mode_t start_mode,
                         uint32_t now_ms)
{
    if (l == NULL) {
        return;
    }
    l->start_mode = (start_mode > PS_LADDER_MAX) ? PS_LADDER_MAX : start_mode;
    if (l->start_mode < l->floor_mode) {
        l->start_mode = l->floor_mode;
    }
    /* An explicit operator change re-arms the ladder: clear the per-rung failure
     * memory so a rung that failed before can be tried again on purpose. */
    l->step_failures[0] = 0u;
    l->step_failures[1] = 0u;
    l->step_failures[2] = 0u;
    ps_ladder_force_mode(l, l->start_mode, now_ms);
}

uint32_t ps_ladder_window_losses(const ps_ladder_t *l)
{
    return l != NULL ? l->losses : 0u;
}

uint32_t ps_ladder_total_losses(const ps_ladder_t *l)
{
    return l != NULL ? l->total_losses : 0u;
}

uint32_t ps_ladder_steps_down(const ps_ladder_t *l)
{
    return l != NULL ? l->steps_down : 0u;
}

uint32_t ps_ladder_steps_up(const ps_ladder_t *l)
{
    return l != NULL ? l->steps_up : 0u;
}
