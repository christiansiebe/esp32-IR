#include "boot_policy.h"

/* Sender confirm window. Kept as plain constants with a host-build fallback so
 * boot_policy.c stays free of ESP-IDF (see test/test_boot_policy.c). */
#ifndef CONFIG_ESPNOW_SENDER_CONFIRM_STABILITY_S
#define CONFIG_ESPNOW_SENDER_CONFIRM_STABILITY_S 180
#endif
#ifndef CONFIG_ESPNOW_SENDER_CONFIRM_GRACE_S
#define CONFIG_ESPNOW_SENDER_CONFIRM_GRACE_S 30
#endif

void boot_policy_defaults(boot_policy_t *policy)
{
    policy->stability_ms = 45u * 1000u;  /* confirm after 45 s up */
    policy->boot_grace_ms = 30u * 1000u; /* give subsystems 30 s to come up */
}

void boot_policy_sender_defaults(boot_policy_t *policy)
{
    /* The sender has no dashboard and nobody watching it, so "app_main got this
     * far" is not proof that the image works: the image that boot-looped every
     * ~74 s on 2026-10-23 got that far every time and was confirmed anyway,
     * which threw away the bootloader safety net and left it looping forever.
     * Staying unconfirmed for the whole window keeps that net armed: any panic
     * inside it marks the image ABORTED, and the next boot falls back to the
     * previous slot. Only an image that stays up for the window is accepted. */
    policy->stability_ms = (uint32_t)CONFIG_ESPNOW_SENDER_CONFIRM_STABILITY_S * 1000u;
    policy->boot_grace_ms = (uint32_t)CONFIG_ESPNOW_SENDER_CONFIRM_GRACE_S * 1000u;
}

void boot_state_reset(boot_state_t *state)
{
    state->decided = false;
}

boot_effect_t boot_action_effect(boot_action_t action)
{
    switch (action) {
    case BOOT_TAKE_CONFIRM:
        return BOOT_EFFECT_CONFIRM;
    case BOOT_TAKE_ROLLBACK:
        return BOOT_EFFECT_ROLLBACK;
    case BOOT_TAKE_NONE:
    default:
        return BOOT_EFFECT_NONE;
    }
}

boot_action_t boot_evaluate(const boot_policy_t *policy, boot_state_t *state,
                            uint32_t uptime_ms, const boot_inputs_t *inputs)
{
    if (state->decided) {
        return BOOT_TAKE_NONE;
    }

    /* Already a normal, confirmed boot: nothing to verify, nothing to roll
     * back. This is the steady state after the first successful boot. */
    if (!inputs->is_pending_verify) {
        state->decided = true;
        return BOOT_TAKE_NONE;
    }

    if (!inputs->dashboard_ok && uptime_ms >= policy->boot_grace_ms) {
        /* The image came up but never became usable: fall back. */
        state->decided = true;
        return BOOT_TAKE_ROLLBACK;
    }

    if (inputs->dashboard_ok && uptime_ms >= policy->stability_ms) {
        state->decided = true;
        return BOOT_TAKE_CONFIRM;
    }

    return BOOT_TAKE_NONE;
}
