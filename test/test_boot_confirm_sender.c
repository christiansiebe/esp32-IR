/* Host test of the SENDER boot-confirm glue.
 *
 * The sender has no dashboard: it confirms its OTA image once its core
 * subsystems are up (boot_confirm_note_ready) and the stability window has
 * passed. This test compiles main/boot_confirm.c with CONFIG_ESPNOW_ROLE_SENDER
 * so the sender path is exercised (the receiver path is covered by
 * test_boot_confirm_trigger.c). The ESP-IDF OTA/system calls are faked so the
 * side effects are observable on the host.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONFIG_ESPNOW_ROLE_SENDER 1

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

static bool ready_at(uint32_t t, uint32_t on_ms)
{
    return on_ms != 0 && t >= on_ms;
}

/* Poll every second for 5 minutes, marking the sender ready per `on_ms`
 * (0 = never ready). Returns the uptime at which a non-NONE effect occurred. */
static uint32_t run(uint32_t ready_on_ms)
{
    for (uint32_t t = 0; t <= 300000u; t += 1000u) {
        boot_confirm_test_set_uptime(t);
        boot_confirm_note_ready(ready_at(t, ready_on_ms));
        boot_confirm_poll();
        if (boot_confirm_test_last_effect() != BOOT_EFFECT_NONE) {
            return t;
        }
    }
    return 0;
}

static void test_sender_never_ready_rolls_back(void)
{
    fake_reset(ESP_OTA_IMG_PENDING_VERIFY);
    CHECK(boot_confirm_init() == ESP_OK, "init ok");
    CHECK(boot_confirm_test_confirmed() == false, "not confirmed before poll");
    uint32_t at = run(0);
    CHECK(at == 30000u, "rollback fires exactly at the grace window");
    CHECK(boot_confirm_test_fail_count() == 1, "rollback triggered exactly once");
    CHECK(boot_confirm_test_confirmed() == false, "never confirmed");
}

static void test_sender_ready_confirms_after_stability(void)
{
    fake_reset(ESP_OTA_IMG_PENDING_VERIFY);
    boot_confirm_init();
    /* Ready 2 s in -> the confirm must wait for the full stability window. */
    uint32_t at = run(2000);
    CHECK(at == 45000u, "confirm fires at the stability window (45 s)");
    CHECK(boot_confirm_test_confirmed() == true, "ready sender is confirmed");
    CHECK(boot_confirm_test_fail_count() == 0, "no rollback on a ready sender");
}

static void test_sender_late_ready_still_rolls_back(void)
{
    /* Ready only after the grace deadline: the image had its one chance. */
    fake_reset(ESP_OTA_IMG_PENDING_VERIFY);
    boot_confirm_init();
    uint32_t at = run(60000);
    CHECK(at == 30000u, "rolls back at the grace deadline before late ready");
    CHECK(boot_confirm_test_fail_count() == 1, "rolled back");
}

static void test_sender_confirmed_image_does_not_roll_back(void)
{
    /* A steady-state boot (image already VALID) must never roll back, even if
     * the sender never reports ready during the boot (e.g. UART worker did not
     * start yet — the sender stays in the previous state). */
    fake_reset(ESP_OTA_IMG_VALID);
    boot_confirm_init();
    uint32_t at = run(0);
    CHECK(at == 0, "no action on a confirmed image");
    CHECK(boot_confirm_test_fail_count() == 0, "no rollback on confirmed image");
}

static void test_sender_health_failure_after_confirm_only_reboots(void)
{
    fake_reset(ESP_OTA_IMG_PENDING_VERIFY);
    boot_confirm_init();
    (void)run(2000); /* healthy: confirmed */
    CHECK(boot_confirm_test_confirmed(), "sender image confirmed first");
    unsigned rollbacks_before = g_rollback_calls;
    boot_confirm_fail("uart wedged");
    CHECK(g_restarts == 1, "confirmed image reboots on a later failure");
    CHECK(g_rollback_calls == rollbacks_before,
          "confirmed image does not attempt a rollback");
}

int main(void)
{
    printf("boot-confirm sender trigger test\n");
    test_sender_never_ready_rolls_back();
    test_sender_ready_confirms_after_stability();
    test_sender_late_ready_still_rolls_back();
    test_sender_confirmed_image_does_not_roll_back();
    test_sender_health_failure_after_confirm_only_reboots();

    printf("boot_confirm_sender: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        return 1;
    }
    return 0;
}
