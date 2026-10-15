/* Host test that the boot-confirm GLUE really triggers recovery.
 *
 * test_boot_policy.c proves the decision (which action). This test proves the
 * side effect: it drives the real main/boot_confirm.c (compiled with
 * BOOT_CONFIRM_TEST_HOOKS) through many failure scenarios and asserts that the
 * rollback is actually triggered — the exact link that was missing when
 * boot_confirm_poll() only handled BOOT_TAKE_CONFIRM and boot_confirm_fail()
 * had no caller.
 *
 * The ESP-IDF OTA/system calls are faked so the run is observable on the host.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boot_confirm.h"
#include "boot_policy.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

/* --- Fakes for the ESP-IDF surface boot_confirm.c links against ----------- */
static esp_ota_img_states_t g_image_state = ESP_OTA_IMG_PENDING_VERIFY;
static unsigned g_marked_valid;
static unsigned g_rollback_calls;
static unsigned g_restarts;

const esp_partition_t *esp_ota_get_running_partition(void)
{
    static esp_partition_t part = {"ota_0"};
    return &part;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,
                                      esp_ota_img_states_t *state)
{
    (void)partition;
    *state = g_image_state;
    return ESP_OK;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++g_marked_valid;
    return ESP_OK;
}

esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    ++g_rollback_calls;
    return ESP_OK;
}

void esp_restart(void)
{
    ++g_restarts;
}

int64_t esp_timer_get_time(void)
{
    return 0;
}

/* --- Test harness --------------------------------------------------------- */
static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        ++g_checks;                                                 \
        if (!(cond)) {                                              \
            ++g_failures;                                           \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
        }                                                           \
    } while (0)

static void fake_reset(esp_ota_img_states_t image_state)
{
    g_image_state = image_state;
    g_marked_valid = 0;
    g_rollback_calls = 0;
    g_restarts = 0;
    boot_confirm_test_reset();
}

static bool dash_up(uint32_t t, uint32_t on_ms, uint32_t off_ms)
{
    if (on_ms == 0 || t < on_ms) {
        return false;
    }
    if (off_ms != 0 && t >= off_ms) {
        return false;
    }
    return true;
}

/* Poll every second for 5 minutes, marking the dashboard per the window, and
 * return the uptime (ms) at which a non-NONE effect was observed (or 0). */
static uint32_t run(uint32_t on_ms, uint32_t off_ms)
{
    for (uint32_t t = 0; t <= 300000u; t += 1000u) {
        boot_confirm_test_set_uptime(t);
        boot_confirm_note_dashboard(dash_up(t, on_ms, off_ms));
        boot_confirm_poll();
        if (boot_confirm_test_last_effect() != BOOT_EFFECT_NONE) {
            return t;
        }
    }
    return 0;
}

typedef struct {
    const char *name;
    uint32_t on_ms;
    uint32_t off_ms;
    boot_effect_t expect;
} scenario_t;

static void test_failure_cases_trigger_rollback(void)
{
    static const scenario_t scenarios[] = {
        {"never comes up",             0,     0,      BOOT_EFFECT_ROLLBACK},
        {"up 2s, dies at 3s",          2000,  3000,   BOOT_EFFECT_ROLLBACK},
        {"up 2s, dies at 20s",         2000,  20000,  BOOT_EFFECT_ROLLBACK},
        {"up 2s, dies at 29s",         2000,  29000,  BOOT_EFFECT_ROLLBACK},
        {"up 2s, dies at 44s",         2000,  44000,  BOOT_EFFECT_ROLLBACK},
        {"comes up only at 31s",       31000, 0,      BOOT_EFFECT_ROLLBACK},
        {"comes up only at 60s",       60000, 0,      BOOT_EFFECT_ROLLBACK},
        {"up 2s and stays",            2000,  0,      BOOT_EFFECT_CONFIRM},
    };
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i) {
        fake_reset(ESP_OTA_IMG_PENDING_VERIFY);
        CHECK(boot_confirm_init() == ESP_OK, "init ok");
        uint32_t at = run(scenarios[i].on_ms, scenarios[i].off_ms);
        boot_effect_t effect = boot_confirm_test_last_effect();
        printf("  glue %-24s -> %-8s at %u.%03us (rollbacks=%u restarts=%u)\n",
               scenarios[i].name,
               effect == BOOT_EFFECT_ROLLBACK ? "ROLLBACK" :
               effect == BOOT_EFFECT_CONFIRM ? "CONFIRM" : "none",
               at / 1000u, at % 1000u,
               boot_confirm_test_fail_count(), g_restarts);
        CHECK(effect == scenarios[i].expect, scenarios[i].name);
        if (scenarios[i].expect == BOOT_EFFECT_ROLLBACK) {
            CHECK(boot_confirm_test_fail_count() == 1,
                  "rollback triggered exactly once");
            CHECK(boot_confirm_test_confirmed() == false,
                  "a rolled-back image is never confirmed");
        } else {
            CHECK(boot_confirm_test_confirmed() == true, "healthy image confirmed");
            CHECK(boot_confirm_test_fail_count() == 0, "healthy image never rolls back");
        }
    }
}

static void test_rollback_at_grace_not_early(void)
{
    /* The failure must be observed at the grace deadline, not before. */
    fake_reset(ESP_OTA_IMG_PENDING_VERIFY);
    boot_confirm_init();
    uint32_t at = run(0, 0);
    CHECK(at == 30000u, "rollback fires exactly at the 30 s grace window");
}

static void test_confirmed_image_does_not_roll_back(void)
{
    /* A steady-state boot (image already VALID) must never roll back, even with
     * no dashboard. */
    fake_reset(ESP_OTA_IMG_VALID);
    boot_confirm_init();
    uint32_t at = run(0, 0);
    CHECK(at == 0, "no action on a confirmed image");
    CHECK(boot_confirm_test_fail_count() == 0, "no rollback on a confirmed image");
}

static void test_health_failure_after_confirm_only_reboots(void)
{
    /* A health failure after the image was confirmed must NOT try to roll back
     * (that slot is already gone); it only reboots. */
    fake_reset(ESP_OTA_IMG_PENDING_VERIFY);
    boot_confirm_init();
    (void)run(2000, 0); /* healthy: confirmed */
    CHECK(boot_confirm_test_confirmed(), "image confirmed first");
    unsigned rollbacks_before = g_rollback_calls;
    boot_confirm_fail("wifi down");
    CHECK(g_restarts == 1, "confirmed image reboots on a later failure");
    CHECK(g_rollback_calls == rollbacks_before,
          "confirmed image does not attempt a rollback");
    CHECK(boot_confirm_test_fail_count() == 0,
          "no rollback effect after confirmation");
}

int main(void)
{
    printf("boot-confirm trigger test\n");
    test_failure_cases_trigger_rollback();
    test_rollback_at_grace_not_early();
    test_confirmed_image_does_not_roll_back();
    test_health_failure_after_confirm_only_reboots();

    printf("boot_confirm_trigger: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
