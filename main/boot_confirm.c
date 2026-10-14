#include "boot_confirm.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "boot_policy.h"

static const char *TAG = "boot_confirm";

/* Only the receiver confirms/rolls back; the sender has no dashboard. */
#if !CONFIG_ESPNOW_ROLE_SENDER

static boot_policy_t g_policy;
static boot_state_t g_state;
static boot_inputs_t g_inputs = {.is_pending_verify = false, .dashboard_ok = false};

static bool g_initialised;
static bool g_confirmed;

#ifdef BOOT_CONFIRM_TEST_HOOKS
/* Host tests drive a virtual uptime and observe the effects. */
static uint32_t g_uptime_override_ms;
static boot_effect_t g_last_effect = BOOT_EFFECT_NONE;
static unsigned g_fail_count;
static const char *g_fail_reason;

static uint32_t boot_uptime_ms(void)
{
    return g_uptime_override_ms;
}

static void boot_effect_confirm(void)
{
    g_last_effect = BOOT_EFFECT_CONFIRM;
    g_confirmed = true;
}

static void boot_effect_rollback(const char *reason)
{
    g_last_effect = BOOT_EFFECT_ROLLBACK;
    ++g_fail_count;
    g_fail_reason = reason;
    /* The real rollback does not return; the test continues so it can assert. */
}
#else
static uint32_t boot_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void boot_effect_confirm(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK || err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        g_confirmed = true;
        ESP_LOGI(TAG, "Image confirmed after %u s up",
                 (unsigned)(boot_uptime_ms() / 1000u));
    } else {
        ESP_LOGW(TAG, "Could not confirm image: %s", esp_err_to_name(err));
    }
}

static void boot_effect_rollback(const char *reason)
{
    boot_confirm_fail(reason);
}
#endif

esp_err_t boot_confirm_init(void)
{
    if (g_initialised) {
        return ESP_OK;
    }
    boot_policy_defaults(&g_policy);
    boot_state_reset(&g_state);
    g_inputs.dashboard_ok = false;
    g_inputs.is_pending_verify = false;
    g_confirmed = false;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        g_inputs.is_pending_verify = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    g_initialised = true;
    ESP_LOGI(TAG, "Image pending verification: %s",
             g_inputs.is_pending_verify ? "yes" : "no");
    return ESP_OK;
}

void boot_confirm_note_dashboard(bool ok)
{
    g_inputs.dashboard_ok = ok;
}

void boot_confirm_poll(void)
{
    if (!g_initialised || g_confirmed) {
        return;
    }
    uint32_t uptime_ms = boot_uptime_ms();
    boot_action_t action = boot_evaluate(&g_policy, &g_state, uptime_ms, &g_inputs);
    switch (boot_action_effect(action)) {
    case BOOT_EFFECT_CONFIRM:
        boot_effect_confirm();
        break;
    case BOOT_EFFECT_ROLLBACK:
        /* The image stayed up but never brought the dashboard up inside the
         * grace window. The bootloader only rolls back on a reset, so an image
         * that comes up but stays unusable must trigger the rollback itself. */
        boot_effect_rollback("dashboard did not come up");
        break;
    case BOOT_EFFECT_NONE:
    default:
        break;
    }
}

void boot_confirm_fail(const char *reason_name)
{
    (void)reason_name;
    if (g_initialised && g_inputs.is_pending_verify && !g_confirmed) {
        ESP_LOGE(TAG, "Health failure (%s) on an unconfirmed image; "
                      "rolling back to the previous firmware",
                 reason_name ? reason_name : "?");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        /* Only returns if the rollback could not be started. */
    }
    ESP_LOGE(TAG, "Health failure (%s); rebooting",
             reason_name ? reason_name : "?");
    esp_restart();
}

#ifdef BOOT_CONFIRM_TEST_HOOKS
/* Test-only accessors (host build only; the firmware never calls them). */
void boot_confirm_test_reset(void)
{
    g_initialised = false;
    g_confirmed = false;
    g_uptime_override_ms = 0;
    g_last_effect = BOOT_EFFECT_NONE;
    g_fail_count = 0;
    g_fail_reason = NULL;
    g_inputs.dashboard_ok = false;
    g_inputs.is_pending_verify = false;
    boot_state_reset(&g_state);
}

void boot_confirm_test_set_uptime(uint32_t uptime_ms)
{
    g_uptime_override_ms = uptime_ms;
}

boot_effect_t boot_confirm_test_last_effect(void)
{
    return g_last_effect;
}

unsigned boot_confirm_test_fail_count(void)
{
    return g_fail_count;
}

const char *boot_confirm_test_fail_reason(void)
{
    return g_fail_reason;
}

bool boot_confirm_test_confirmed(void)
{
    return g_confirmed;
}
#endif

#else /* CONFIG_ESPNOW_ROLE_SENDER */

/* The sender has no dashboard and (currently) no boot-confirmation policy, so
 * these are inert: the sender keeps the pre-existing boot behaviour. When the
 * sender gets its own confirm criterion, implement it here. */
esp_err_t boot_confirm_init(void) { return ESP_OK; }
void boot_confirm_note_dashboard(bool ok) { (void)ok; }
void boot_confirm_poll(void) { }
void boot_confirm_fail(const char *reason_name) { (void)reason_name; }

#endif
