#include "wall_clock.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define WALL_CLOCK_NAMESPACE "wallclock"
#define WALL_CLOCK_KEY       "last"

static const char *TAG = "wall_clock";

esp_err_t wall_clock_store_save(int64_t utc_seconds)
{
    /* Refuse a nonsense clock: the store must never hold a value the next boot
     * would seed from and then treat as "recent". */
    if (!wall_clock_utc_is_sane(utc_seconds)) {
        return ESP_ERR_INVALID_ARG;
    }
    wall_clock_record_t record;
    wall_clock_make_record(&record, utc_seconds);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WALL_CLOCK_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, WALL_CLOCK_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wall_clock_store_load(int64_t *utc_seconds, bool *found)
{
    if (utc_seconds != NULL) {
        *utc_seconds = 0;
    }
    if (found != NULL) {
        *found = false;
    }
    if (utc_seconds == NULL || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WALL_CLOCK_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    wall_clock_record_t record = {0};
    size_t length = sizeof(record);
    err = nvs_get_blob(handle, WALL_CLOCK_KEY, &record, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(record) || !wall_clock_record_is_valid(&record)) {
        ESP_LOGW(TAG, "Stored wall clock is corrupt; ignoring it");
        return ESP_OK;
    }
    *utc_seconds = record.utc_seconds;
    *found = true;
    return ESP_OK;
}
