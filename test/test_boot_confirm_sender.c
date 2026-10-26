/* Host test for the SENDER half of the boot-confirm glue.
 *
 * test_boot_confirm_trigger.c covers the receiver policy. This compiles the
 * real main/boot_confirm.c with CONFIG_ESPNOW_ROLE_SENDER=1 and proves the
 * sender now uses the confirm/rollback machinery instead of confirming the
 * image in app_main:
 *
 *   - a stack that never comes up rolls back at the grace deadline
 *   - a stack that comes up is NOT confirmed before the long sender window,
 *     so an image that resets every ~74 s (the 2026-10-23 boot loop) is never
 *     accepted and the bootloader keeps falling back
 *   - a genuinely stable image is confirmed once the window has passed
 *
 * The ESP-IDF OTA/system calls are faked so the run is observable on the host.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

static void fake_reset(void)
{
    g_image_state = ESP_OTA_IMG_PENDING_VERIFY;
    g_marked_valid = 0;
    g_rollback_calls = 0;
    g_restarts = 0;
    boot_confirm_test_reset();
}

/* Simulate ONE boot attempt: poll every second for `run_ms`, reporting the
 * stack as up from `ready_ms` (0 = never). Returns the uptime of the first
 * non-NONE effect, or 0 if the run ended undecided. */
static uint32_t run_boot(uint32_t ready_ms, uint32_t run_ms)
{
    for (uint32_t t = 0; t <= run_ms; t += 1000u) {
        boot_confirm_test_set_uptime(t);
        boot_confirm_note_dashboard(ready_ms != 0 && t >= ready_ms);
        boot_confirm_poll();
        if (boot_confirm_test_last_effect() != BOOT_EFFECT_NONE) {
            return t;
        }
    }
    return 0;
}

static void test_stack_never_up_rolls_back(void)
{
    fake_reset();
    boot_confirm_init();
    uint32_t at = run_boot(0, 300000);
    CHECK(boot_confirm_test_last_effect() == BOOT_EFFECT_ROLLBACK,
          "sender with no working stack rolls back");
    CHECK(at == 30000u, "sender rollback fires at the 30 s grace window");
    CHECK(boot_confirm_test_fail_count() == 1, "rollback triggered exactly once");
}

static void test_boot_loop_is_never_confirmed(void)
{
    /* The observed failure: the image came up every time and reset ~74 s
     * later. One full loop period must end undecided (unconfirmed), so the
     * bootloader marks it ABORTED and falls back on the next boot. */
    fake_reset();
    boot_confirm_init();
    uint32_t at = run_boot(2000, 74000);
    CHECK(at == 0, "a boot that resets at 74 s is never confirmed");
    CHECK(boot_confirm_test_confirmed() == false, "looping image stays unconfirmed");
}

static void test_stable_sender_confirms_after_window(void)
{
    fake_reset();
    boot_confirm_init();
    uint32_t at = run_boot(2000, 300000);
    CHECK(boot_confirm_test_last_effect() == BOOT_EFFECT_CONFIRM,
          "a stable sender image is confirmed");
    CHECK(at >= 180000u, "confirmation waits for the full sender window");
    CHECK(boot_confirm_test_confirmed() == true, "the image is confirmed exactly once");
    CHECK(boot_confirm_test_fail_count() == 0, "a healthy image never rolls back");
}

static void test_fail_after_confirm_only_reboots(void)
{
    fake_reset();
    boot_confirm_init();
    (void)run_boot(2000, 300000); /* confirmed */
    CHECK(boot_confirm_test_confirmed(), "image confirmed first");
    unsigned rollbacks_before = g_rollback_calls;
    boot_confirm_fail("uart wedge");
    CHECK(g_restarts == 1, "confirmed image reboots on a later failure");
    CHECK(g_rollback_calls == rollbacks_before,
          "confirmed image does not attempt a rollback");
}

int main(void)
{
    printf("boot-confirm sender trigger test\n");
    test_stack_never_up_rolls_back();
    test_boot_loop_is_never_confirmed();
    test_stable_sender_confirms_after_window();
    test_fail_after_confirm_only_reboots();

    printf("boot_confirm_sender: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
