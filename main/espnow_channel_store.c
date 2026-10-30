#include "espnow_channel_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define ESPNOW_CHANNEL_NAMESPACE "espnowch"
#define ESPNOW_CHANNEL_KEY       "last"

static const char *TAG = "espnow_channel";

/* Same CRC-32 (reflected 0xEDB88320) as the power-history and wall-clock
 * records, so every on-disk format is validated the same way. */
static uint32_t crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

bool espnow_channel_is_valid(uint8_t channel)
{
    return channel >= ESPNOW_CHANNEL_MIN && channel <= ESPNOW_CHANNEL_MAX;
}

void espnow_channel_make_record(espnow_channel_record_t *record, uint8_t channel)
{
    if (record == NULL) {
        return;
    }
    memset(record, 0, sizeof(*record));
    record->magic = ESPNOW_CHANNEL_MAGIC;
    record->version = ESPNOW_CHANNEL_VERSION;
    record->channel = channel;
    record->checksum = crc32((const uint8_t *)record,
                             offsetof(espnow_channel_record_t, checksum));
}

bool espnow_channel_record_is_valid(const espnow_channel_record_t *record)
{
    if (record == NULL) {
        return false;
    }
    if (record->magic != ESPNOW_CHANNEL_MAGIC ||
        record->version != ESPNOW_CHANNEL_VERSION) {
        return false;
    }
    if (!espnow_channel_is_valid(record->channel)) {
        return false;
    }
    return record->checksum ==
           crc32((const uint8_t *)record, offsetof(espnow_channel_record_t, checksum));
}

esp_err_t espnow_channel_store_save(uint8_t channel)
{
    /* Refuse an out-of-range channel: the store must never hold a value the
     * next boot would seed the radio with and then fail to start ESP-NOW on. */
    if (!espnow_channel_is_valid(channel)) {
        return ESP_ERR_INVALID_ARG;
    }
    espnow_channel_record_t record;
    espnow_channel_make_record(&record, channel);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPNOW_CHANNEL_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, ESPNOW_CHANNEL_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t espnow_channel_store_load(uint8_t *channel, bool *found)
{
    if (channel != NULL) {
        *channel = 0;
    }
    if (found != NULL) {
        *found = false;
    }
    if (channel == NULL || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPNOW_CHANNEL_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    espnow_channel_record_t record = {0};
    size_t length = sizeof(record);
    err = nvs_get_blob(handle, ESPNOW_CHANNEL_KEY, &record, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(record) || !espnow_channel_record_is_valid(&record)) {
        ESP_LOGW(TAG, "Stored ESP-NOW channel is corrupt; ignoring it");
        return ESP_OK;
    }
    *channel = record.channel;
    *found = true;
    return ESP_OK;
}
