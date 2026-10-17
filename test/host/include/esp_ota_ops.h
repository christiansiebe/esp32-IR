/* Host-test shim: the slice of esp_ota_ops.h that boot_confirm.c uses. The
 * functions are not defined here; the boot-confirm host test supplies fakes so
 * it can observe whether the glue really rolls back on a failure scenario. */
#pragma once
#include "esp_shim.h"

typedef struct {
    const char *label;
} esp_partition_t;

typedef enum {
    ESP_OTA_IMG_NEW = 0,
    ESP_OTA_IMG_PENDING_VERIFY = 1,
    ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3,
    ESP_OTA_IMG_ABORTED = 4,
    ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFF
} esp_ota_img_states_t;

#define ESP_ERR_OTA_ROLLBACK_INVALID_STATE 0x1503

const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,
                                      esp_ota_img_states_t *state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
