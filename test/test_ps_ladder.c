/* Adaptive modem-sleep ladder: start frugal (max), step down when a window
 * loses too many frames, climb back after a run of clean windows, never below
 * the floor, never above the start, and never judge a quiet window. Pure logic,
 * no ESP-IDF. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ps_ladder.h"

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

#define WIN_MS 60000u
#define LOSS_PCT 20u
#define MIN_FRAMES 4u
#define MIN_LOSSES 2u
#define RECOVERY 3u

static void init(ps_ladder_t *l, ps_ladder_mode_t floor, ps_ladder_mode_t start)
{
    ps_ladder_init(l, WIN_MS, LOSS_PCT, MIN_FRAMES, MIN_LOSSES, RECOVERY, floor,
                   start, 0);
}

/* Advance one window, feeding `losses` out of `frames` first. */
static ps_ladder_mode_t window(ps_ladder_t *l, uint32_t *now, uint32_t losses,
                               uint32_t frames)
{
    ps_ladder_note_frames(l, frames);
    ps_ladder_note_loss(l, losses);
    *now += WIN_MS;
    return ps_ladder_poll(l, *now);
}

int main(void)
{
    /* Starts at the frugal rung. */
    {
        ps_ladder_t l;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MAX, "starts at max");
        CHECK(ps_ladder_enabled(&l), "enabled by default");
    }

    /* A single stray loss below the step floor does not move a rung. */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        CHECK(window(&l, &now, 1, 100) == PS_LADDER_MAX, "stray loss keeps max");
        CHECK(ps_ladder_steps_down(&l) == 0, "no step for a stray loss");
    }

    /* A lossy window steps max -> min. */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        CHECK(window(&l, &now, 50, 100) == PS_LADDER_MIN, "50% loss steps max->min");
        CHECK(ps_ladder_steps_down(&l) == 1, "one step down");
    }

    /* A quiet window (too few frames) is not judged even with a loss. */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        CHECK(window(&l, &now, 1, MIN_FRAMES - 1) == PS_LADDER_MAX,
              "quiet window not judged");
        CHECK(ps_ladder_steps_down(&l) == 0, "no step on a quiet window");
    }

    /* Sustained loss steps min -> off (with the floor at off). */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        CHECK(window(&l, &now, 50, 100) == PS_LADDER_MIN, "max->min");
        CHECK(window(&l, &now, 50, 100) == PS_LADDER_OFF, "min->off");
        CHECK(window(&l, &now, 50, 100) == PS_LADDER_OFF, "off is the floor");
        CHECK(ps_ladder_steps_down(&l) == 2, "two steps down total");
    }

    /* The floor can be raised to MIN so the ladder never ends up at off. */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_MIN, PS_LADDER_MAX);
        for (int i = 0; i < 10; ++i) {
            window(&l, &now, 50, 100);
        }
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN, "floor min is respected");
    }

    /* After a run of clean windows it climbs back, but never above the start. */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        window(&l, &now, 50, 100); /* -> min */
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN, "at min");
        ps_ladder_mode_t m = PS_LADDER_MIN;
        for (uint32_t i = 0; i < RECOVERY; ++i) {
            m = window(&l, &now, 0, 100);
        }
        CHECK(m == PS_LADDER_MAX, "clean streak climbs back to max");
        CHECK(ps_ladder_steps_up(&l) == 1, "one step up");
        /* More clean windows must not climb above the start. */
        for (uint32_t i = 0; i < RECOVERY * 2; ++i) {
            m = window(&l, &now, 0, 100);
        }
        CHECK(m == PS_LADDER_MAX, "never climbs above the start");
        CHECK(ps_ladder_steps_up(&l) == 1, "no extra climb above start");
    }

    /* A non-clean but non-tripping window resets the clean streak (no climb). */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        window(&l, &now, 50, 100); /* -> min */
        for (uint32_t i = 0; i < RECOVERY - 1; ++i) {
            window(&l, &now, 0, 100);
        }
        window(&l, &now, 1, 100); /* a stray loss breaks the streak */
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN, "streak broken, still min");
        for (uint32_t i = 0; i < RECOVERY - 1; ++i) {
            window(&l, &now, 0, 100);
        }
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN, "not enough clean windows yet");
    }

    /* A partial window does not evaluate early. */
    {
        ps_ladder_t l;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        ps_ladder_note_frames(&l, 100);
        ps_ladder_note_loss(&l, 100);
        CHECK(ps_ladder_poll(&l, WIN_MS - 1) == PS_LADDER_MAX,
              "window not elapsed, no change");
    }

    /* Disabled: poll reports the mode unchanged even with heavy loss. */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        ps_ladder_set_enabled(&l, false);
        CHECK(!ps_ladder_enabled(&l), "disabled");
        for (int i = 0; i < 5; ++i) {
            window(&l, &now, 50, 100);
        }
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MAX, "disabled does not adapt");
    }

    /* force_mode clamps to the floor and clears the window. */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_MIN, PS_LADDER_MAX);
        ps_ladder_note_loss(&l, 9);
        ps_ladder_force_mode(&l, PS_LADDER_OFF, now); /* below floor -> min */
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN, "force clamps to floor");
        CHECK(ps_ladder_window_losses(&l) == 0, "force clears the window");
    }

    /* A start below the floor is raised to the floor. */
    {
        ps_ladder_t l;
        init(&l, PS_LADDER_MIN, PS_LADDER_OFF);
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN, "start clamped up to floor");
    }

    /* A rung that failed twice is not climbed back to: no oscillation. */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        window(&l, &now, 50, 100); /* max fails -> min, failure[2]=1 */
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN, "down to min");
        /* climb back to max */
        for (uint32_t i = 0; i < RECOVERY; ++i) {
            window(&l, &now, 0, 100);
        }
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MAX, "climbed back to max");
        window(&l, &now, 50, 100); /* max fails again -> min, failure[2]=2 */
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN, "down to min again");
        /* now clean windows must NOT climb back to the twice-failed max */
        for (uint32_t i = 0; i < RECOVERY * 3; ++i) {
            window(&l, &now, 0, 100);
        }
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MIN,
              "twice-failed rung is not retried (no oscillation)");
        CHECK(ps_ladder_steps_down(&l) == 2, "exactly two step-downs");
    }

    /* An explicit operator change re-arms the ladder (clears failure memory). */
    {
        ps_ladder_t l;
        uint32_t now = 0;
        init(&l, PS_LADDER_OFF, PS_LADDER_MAX);
        window(&l, &now, 50, 100);
        window(&l, &now, 0, 100);
        window(&l, &now, 50, 100); /* max failed twice */
        ps_ladder_set_start(&l, PS_LADDER_MAX, now); /* operator re-arms */
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MAX, "operator re-armed to max");
        for (uint32_t i = 0; i < RECOVERY; ++i) {
            window(&l, &now, 0, 100);
        }
        CHECK(ps_ladder_mode(&l) == PS_LADDER_MAX, "stays at re-armed max");
    }

    printf("ps_ladder: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
