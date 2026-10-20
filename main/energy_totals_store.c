#include "energy_totals.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define ENERGY_TOTALS_NAMESPACE "energy"
#define ENERGY_TOTALS_KEY       "totals"

static const char *TAG = "energy_totals";

esp_err_t energy_totals_store_save(const energy_totals_t *totals)
{
    if (totals == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    energy_totals_blob_t blob;
    energy_totals_make_blob(totals, &blob);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ENERGY_TOTALS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, ENERGY_TOTALS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t energy_totals_store_load(energy_totals_t *totals, bool *found)
{
    if (found != NULL) {
        *found = false;
    }
    if (totals == NULL || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    energy_totals_reset(totals);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ENERGY_TOTALS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    energy_totals_blob_t blob = {0};
    size_t length = sizeof(blob);
    err = nvs_get_blob(handle, ENERGY_TOTALS_KEY, &blob, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(blob) || !energy_totals_from_blob(&blob, totals)) {
        ESP_LOGW(TAG, "Stored energy totals are corrupt; starting fresh");
        energy_totals_reset(totals);
        return ESP_OK;
    }
    *found = true;
    return ESP_OK;
}
