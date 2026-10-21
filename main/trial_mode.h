/* Trial mode: apply a risky setting, then auto-revert unless it proves itself.
 *
 * The runtime knobs (CPU clock, light sleep, ESP-NOW retries) can brick the
 * radio - a 40 MHz DFS ceiling crashed the sender, a wrong retry count starves
 * the link. Testing such a value on the live device is only safe if a bad value
 * reverts by itself, without a hand on the reset button.
 *
 * The rule is a one-boot contract:
 *   - 'set trial ...' stores the candidate next to the committed config, applies
 *     it live and marks the trial pending in NVS.
 *   - The device must stay demonstrably functional for the trial window. When
 *     that window elapses with a healthy link, the candidate is committed.
 *   - A health failure inside the window is acted on immediately: the caller
 *     reboots, and the boot that follows sees the still-pending marker, so it
 *     reverts to the committed config instead of re-applying the candidate.
 *     (A crash/watchdog reset during the window lands in exactly the same path,
 *     so even a hang that never reaches the health check reverts.)
 *
 * The decision logic is pure C (no ESP-IDF) so it is host-tested
 * (test/test_trial_mode.c); main.c owns the NVS marker and the actions.
 */
#ifndef TRIAL_MODE_H
#define TRIAL_MODE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TRIAL_ACT_NONE = 0,  /* keep running the trial */
    TRIAL_ACT_COMMIT,    /* window survived: keep the candidate */
    TRIAL_ACT_REVERT,    /* something is wrong: reboot to drop the candidate */
} trial_action_t;

typedef struct {
    uint32_t window_ms;   /* healthy time that commits the candidate */
    uint32_t grace_ms;    /* uptime before the health signal is trusted */
} trial_policy_t;

typedef struct {
    trial_policy_t policy;
    uint64_t started_at_ms;
    bool pending;        /* a trial is being evaluated */
    bool saw_healthy;    /* at least one healthy sample inside the window */
    bool decided;        /* commit/revert already returned (final) */
} trial_t;

/* 60 s of healthy running commits; the first 10 s are grace, because a Wi-Fi
 * association and the first DHCP lease take a moment and must not be mistaken
 * for a broken setting. */
void trial_policy_defaults(trial_policy_t *policy);

/* Start evaluating a trial that began at `now_ms` (the apply instant). */
void trial_begin(trial_t *t, const trial_policy_t *policy, uint64_t now_ms);

/* Evaluate once. `healthy` is the caller's "the device still works" signal
 * (link up + heap above the floor). Returns the action to take. Once a trial is
 * decided the result is final. */
trial_action_t trial_tick(trial_t *t, uint64_t now_ms, bool healthy);

/* Whether a trial that was pending when the device booted must be reverted.
 * The candidate is never re-applied after a reset that happened mid-trial, so
 * this is the whole rollback: true -> drop the candidate and keep the committed
 * config. Pure, so the boot path is host-testable. */
bool trial_boot_should_revert(bool pending_at_boot);

#endif /* TRIAL_MODE_H */
