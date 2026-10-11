#include "crash_journal.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#define CRASH_JOURNAL_NAMESPACE "crashlog"
#define CRASH_JOURNAL_KEY       "journal"

static const char *TAG = "crash_journal";

esp_err_t crash_journal_save(const crash_journal_t *journal)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CRASH_JOURNAL_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, CRASH_JOURNAL_KEY, journal, sizeof(*journal));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t crash_journal_load(crash_journal_t *journal, bool *found)
{
    *found = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CRASH_JOURNAL_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    size_t length = sizeof(*journal);
    err = nvs_get_blob(handle, CRASH_JOURNAL_KEY, journal, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(*journal) || !crash_journal_is_valid(journal)) {
        ESP_LOGW(TAG, "Stored crash journal is corrupt; ignoring it");
        return ESP_OK;
    }
    *found = true;
    return ESP_OK;
}
