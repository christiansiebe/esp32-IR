#include "power_history.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define POWER_HISTORY_MAGIC 0x50484C31u
#define POWER_HISTORY_VERSION 2u
#define POWER_HISTORY_MOUNT_PATH "/powerlog"
#define POWER_HISTORY_FILE POWER_HISTORY_MOUNT_PATH "/quarters.bin"
#define POWER_HISTORY_PARTITION "storage"
#define POWER_HISTORY_QUEUE_LENGTH 16u
#define POWER_HISTORY_TASK_STACK 4096u
#define POWER_HISTORY_BATCH_RECORDS 5u
#define POWER_HISTORY_BATCH_TIMEOUT_MS 60000u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;
    uint32_t next_sequence;
    uint32_t record_count;
    uint32_t next_slot;
    uint32_t checksum;
} power_history_header_t;

static const char *TAG = "power_history";
static FILE *g_history_file;
static power_history_header_t g_history_header;
static uint8_t g_active_header_copy;
static QueueHandle_t g_history_queue;
static SemaphoreHandle_t g_history_mutex;
static uint32_t g_queue_drops;
static uint32_t g_write_errors;
static bool g_history_ready;
static bool g_history_ram_fallback;
static power_history_record_t g_history_ram_records[POWER_HISTORY_PAGE_SIZE];
static size_t g_history_ram_count;
static portMUX_TYPE g_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t header_checksum(const power_history_header_t *header)
{
    const uint8_t *bytes = (const uint8_t *)header;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < offsetof(power_history_header_t, checksum); ++i) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

static uint32_t record_checksum(const power_history_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < offsetof(power_history_record_t, checksum); ++i) {
        crc ^= bytes[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

static bool record_is_valid(const power_history_record_t *record)
{
    return record->sequence != 0 && record->format_version == POWER_HISTORY_VERSION &&
           record->checksum == record_checksum(record);
}

static bool header_is_valid(const power_history_header_t *header)
{
    return header->magic == POWER_HISTORY_MAGIC &&
           header->version == POWER_HISTORY_VERSION &&
           header->next_sequence != 0 &&
           header->record_count <= POWER_HISTORY_CAPACITY &&
           header->next_slot < POWER_HISTORY_CAPACITY &&
           header->checksum == header_checksum(header);
}

static bool history_partition_is_erased(void)
{
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                 POWER_HISTORY_PARTITION);
    if (partition == NULL) {
        ESP_LOGE(TAG, "SPIFFS partition '%s' was not found",
                 POWER_HISTORY_PARTITION);
        return false;
    }

    uint8_t buffer[256];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(buffer)) {
        size_t chunk_size = partition->size - offset;
        if (chunk_size > sizeof(buffer)) {
            chunk_size = sizeof(buffer);
        }
        esp_err_t err = esp_partition_read(partition, offset, buffer, chunk_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not inspect SPIFFS partition: %s",
                     esp_err_to_name(err));
            return false;
        }
        for (size_t i = 0; i < chunk_size; ++i) {
            if (buffer[i] != 0xFF) {
                return false;
            }
        }
    }
    return true;
}

static bool write_header_copy(uint8_t copy,
                              const power_history_header_t *header)
{
    long offset = (long)(copy * sizeof(*header));
    return fseek(g_history_file, offset, SEEK_SET) == 0 &&
           fwrite(header, sizeof(*header), 1, g_history_file) == 1 &&
           fflush(g_history_file) == 0 &&
           fsync(fileno(g_history_file)) == 0;
}

static bool write_records(power_history_record_t *records, size_t record_count)
{
    power_history_header_t updated = g_history_header;
    for (size_t i = 0; i < record_count; ++i) {
        records[i].sequence = updated.next_sequence;
        records[i].format_version = POWER_HISTORY_VERSION;
        records[i].reserved = 0;
        records[i].checksum = record_checksum(&records[i]);
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             updated.next_slot * sizeof(records[i]));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fwrite(&records[i], sizeof(records[i]), 1, g_history_file) != 1) {
            return false;
        }
        ++updated.next_sequence;
        if (updated.next_sequence == 0) {
            updated.next_sequence = 1;
        }
        if (updated.record_count < POWER_HISTORY_CAPACITY) {
            ++updated.record_count;
        }
        updated.next_slot = (updated.next_slot + 1u) % POWER_HISTORY_CAPACITY;
    }
    if (fflush(g_history_file) != 0 || fsync(fileno(g_history_file)) != 0) {
        return false;
    }
    ++updated.generation;
    updated.checksum = header_checksum(&updated);

    uint8_t next_copy = (uint8_t)(1u - g_active_header_copy);
    if (!write_header_copy(next_copy, &updated)) {
        return false;
    }
    g_history_header = updated;
    g_active_header_copy = next_copy;
    return true;
}

static void power_history_worker(void *arg)
{
    (void)arg;
    power_history_record_t records[POWER_HISTORY_BATCH_RECORDS];
    while (xQueueReceive(g_history_queue, &records[0], portMAX_DELAY) == pdTRUE) {
        size_t record_count = 1;
        while (record_count < POWER_HISTORY_BATCH_RECORDS &&
               xQueueReceive(g_history_queue, &records[record_count],
                             pdMS_TO_TICKS(POWER_HISTORY_BATCH_TIMEOUT_MS)) == pdTRUE) {
            ++record_count;
        }
        if (xSemaphoreTake(g_history_mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Could not lock persistent history");
            portENTER_CRITICAL(&g_stats_lock);
            ++g_write_errors;
            portEXIT_CRITICAL(&g_stats_lock);
            continue;
        }
        bool persist_ok = true;
        if (g_history_file != NULL) {
            persist_ok = write_records(records, record_count);
            if (!persist_ok) {
                portENTER_CRITICAL(&g_stats_lock);
                ++g_write_errors;
                portEXIT_CRITICAL(&g_stats_lock);
                ESP_LOGE(TAG, "Could not persist power-history batch");
            }
        }
        if (persist_ok || g_history_file == NULL) {
            for (size_t i = 0; i < record_count; ++i) {
                if (g_history_file == NULL) {
                    /* RAM-Fallback: write_records() wurde nicht aufgerufen,
                     * daher Sequenz/Version/Checksum hier vergeben. Ohne
                     * Sequenz bleibt sequence=0 und power_history_read()
                     * verwirft jeden Datensatz (Filter sequence > after_sequence). */
                    records[i].sequence = g_history_header.next_sequence;
                    records[i].format_version = POWER_HISTORY_VERSION;
                    records[i].reserved = 0;
                    records[i].checksum = record_checksum(&records[i]);
                    if (++g_history_header.next_sequence == 0) {
                        g_history_header.next_sequence = 1;
                    }
                    if (g_history_header.record_count < POWER_HISTORY_PAGE_SIZE) {
                        ++g_history_header.record_count;
                    }
                }
                if (g_history_ram_count < POWER_HISTORY_PAGE_SIZE) {
                    g_history_ram_records[g_history_ram_count++] = records[i];
                } else {
                    memmove(&g_history_ram_records[0], &g_history_ram_records[1],
                            (POWER_HISTORY_PAGE_SIZE - 1) * sizeof(records[0]));
                    g_history_ram_records[POWER_HISTORY_PAGE_SIZE - 1] = records[i];
                }
            }
        }
        xSemaphoreGive(g_history_mutex);
    }
}

esp_err_t power_history_init(void)
{
    if (g_history_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    g_history_mutex = xSemaphoreCreateMutex();
    if (g_history_mutex == NULL) {
        ESP_LOGE(TAG, "Could not create history mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_spiffs_conf_t config = {
        .base_path = POWER_HISTORY_MOUNT_PATH,
        .partition_label = POWER_HISTORY_PARTITION,
        .max_files = 1,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&config);
    bool storage_available = true;
    if (err != ESP_OK) {
        if (!history_partition_is_erased()) {
            ESP_LOGE(TAG, "Could not mount persistent SPIFFS partition: %s; "
                          "continuing with RAM history, preserving existing partition contents",
                     esp_err_to_name(err));
            storage_available = false;
        } else {
            ESP_LOGW(TAG, "Formatting the blank SPIFFS partition for first use");
            err = esp_spiffs_format(POWER_HISTORY_PARTITION);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not initialize blank SPIFFS partition: %s; "
                              "continuing with RAM history",
                         esp_err_to_name(err));
                storage_available = false;
            } else {
                err = esp_vfs_spiffs_register(&config);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not mount newly formatted SPIFFS partition: %s; "
                                  "continuing with RAM history",
                             esp_err_to_name(err));
                    storage_available = false;
                }
            }
        }
    }

    g_history_file = NULL;
    if (storage_available) {
        g_history_file = fopen(POWER_HISTORY_FILE, "r+b");
        if (g_history_file == NULL) {
            g_history_file = fopen(POWER_HISTORY_FILE, "w+b");
            if (g_history_file == NULL) {
                ESP_LOGE(TAG, "Could not create persistent history file; continuing with RAM history");
                storage_available = false;
            }
        }
    }
    if (g_history_file == NULL) {
        memset(&g_history_header, 0, sizeof(g_history_header));
        g_history_header.magic = POWER_HISTORY_MAGIC;
        g_history_header.version = POWER_HISTORY_VERSION;
        g_history_header.generation = 1;
        g_history_header.next_sequence = 1;
        g_history_header.checksum = header_checksum(&g_history_header);
        g_active_header_copy = 1;
    } else if (fseek(g_history_file, 0, SEEK_END) != 0 || ftell(g_history_file) == 0) {
        memset(&g_history_header, 0, sizeof(g_history_header));
        power_history_header_t previous = g_history_header;
        previous.generation = 0;
        previous.checksum = header_checksum(&previous);
        if (!write_header_copy(0, &previous) ||
            !write_header_copy(1, &g_history_header)) {
            ESP_LOGE(TAG, "Could not initialize persistent history metadata; continuing with RAM history");
            fclose(g_history_file);
            g_history_file = NULL;
            memset(&g_history_header, 0, sizeof(g_history_header));
            g_history_header.magic = POWER_HISTORY_MAGIC;
            g_history_header.version = POWER_HISTORY_VERSION;
            g_history_header.generation = 1;
            g_history_header.next_sequence = 1;
            g_history_header.checksum = header_checksum(&g_history_header);
            g_active_header_copy = 1;
        } else {
            g_active_header_copy = 1;
        }
    } else {
        power_history_header_t headers[2] = {0};
        bool valid[2] = {false, false};
        for (uint8_t i = 0; i < 2; ++i) {
            if (fseek(g_history_file, (long)(i * sizeof(headers[i])), SEEK_SET) == 0 &&
                fread(&headers[i], sizeof(headers[i]), 1, g_history_file) == 1) {
                valid[i] = header_is_valid(&headers[i]);
            }
        }
        if (!valid[0] && !valid[1]) {
            ESP_LOGE(TAG, "Persistent history metadata is corrupt; using RAM history and preserving partition");
            fclose(g_history_file);
            g_history_file = NULL;
            memset(&g_history_header, 0, sizeof(g_history_header));
            g_history_header.magic = POWER_HISTORY_MAGIC;
            g_history_header.version = POWER_HISTORY_VERSION;
            g_history_header.generation = 1;
            g_history_header.next_sequence = 1;
            g_history_header.checksum = header_checksum(&g_history_header);
            g_active_header_copy = 1;
        } else if (valid[0] && valid[1]) {
            g_active_header_copy =
                (int32_t)(headers[1].generation - headers[0].generation) > 0 ? 1 : 0;
            g_history_header = headers[g_active_header_copy];
        } else {
            g_active_header_copy = valid[1] ? 1 : 0;
            g_history_header = headers[g_active_header_copy];
        }
    }

    g_history_queue = xQueueCreate(POWER_HISTORY_QUEUE_LENGTH,
                                   sizeof(power_history_record_t));
    if (g_history_queue == NULL) {
        ESP_LOGE(TAG, "Could not create persistent history queue");
        return ESP_ERR_NO_MEM;
    }
    g_history_ram_fallback = true;
    g_history_ram_count = 0;
    g_history_ready = true;
    BaseType_t task_result = xTaskCreate(power_history_worker, "power_history",
                                         POWER_HISTORY_TASK_STACK, NULL, 4, NULL);
    if (task_result != pdPASS) {
        g_history_ready = false;
        g_history_ram_fallback = false;
        ESP_LOGE(TAG, "Could not start persistent history writer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Persistent history ready: %lu of %u minute records",
             (unsigned long)g_history_header.record_count,
             POWER_HISTORY_CAPACITY);
    return ESP_OK;
}

bool power_history_enqueue(const power_history_record_t *record)
{
    if (!g_history_ready || record == NULL ||
        xQueueSend(g_history_queue, record, 0) != pdTRUE) {
        portENTER_CRITICAL(&g_stats_lock);
        ++g_queue_drops;
        portEXIT_CRITICAL(&g_stats_lock);
        ESP_LOGE(TAG, "Completed interval could not be queued for persistence");
        return false;
    }
    return true;
}

void power_history_get_status(power_history_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->ready = g_history_ready;
    if (g_history_ready && xSemaphoreTake(g_history_mutex, portMAX_DELAY) == pdTRUE) {
        status->record_count = g_history_header.record_count;
        status->latest_sequence = g_history_header.next_sequence - 1u;
        if (status->record_count != 0) {
            status->oldest_sequence =
                g_history_header.next_sequence - status->record_count;
            if (status->oldest_sequence == 0) {
                status->oldest_sequence = 1;
            }
        }
        xSemaphoreGive(g_history_mutex);
    }
    portENTER_CRITICAL(&g_stats_lock);
    status->queue_drops = g_queue_drops;
    status->write_errors = g_write_errors;
    portEXIT_CRITICAL(&g_stats_lock);
}

esp_err_t power_history_read(uint32_t after_sequence, uint32_t recent_count,
                             power_history_record_t *records, size_t capacity,
                             size_t *record_count, power_history_status_t *status,
                             bool *overflow)
{
    if (records == NULL || capacity == 0 || record_count == NULL ||
        status == NULL || overflow == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *record_count = 0;
    *overflow = false;
    power_history_get_status(status);
    if (!status->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_history_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    status->record_count = g_history_header.record_count;
    status->latest_sequence = g_history_header.next_sequence - 1u;
    status->oldest_sequence = status->record_count == 0 ? 0 :
                              g_history_header.next_sequence - status->record_count;
    if (status->oldest_sequence == 0 && status->record_count != 0) {
        status->oldest_sequence = 1;
    }

    uint32_t oldest_sequence = status->oldest_sequence;
    uint32_t latest_sequence = status->latest_sequence;
    if (recent_count != 0 && status->record_count != 0) {
        uint32_t recent_after = latest_sequence >= recent_count
                                    ? latest_sequence - recent_count
                                    : 0;
        if (recent_after > after_sequence) {
            after_sequence = recent_after;
        }
    }
    if (oldest_sequence != 0 && after_sequence < oldest_sequence - 1u) {
        *overflow = true;
        after_sequence = oldest_sequence - 1u;
    }

    if (g_history_file == NULL) {
        for (size_t i = 0; i < g_history_ram_count; ++i) {
            const power_history_record_t *record = &g_history_ram_records[i];
            if (record->sequence > after_sequence) {
                records[*record_count] = *record;
                ++*record_count;
                if (*record_count == capacity) {
                    break;
                }
            }
        }
        xSemaphoreGive(g_history_mutex);
        return ESP_OK;
    }

    uint32_t oldest_slot =
        (g_history_header.next_slot + POWER_HISTORY_CAPACITY -
         g_history_header.record_count) % POWER_HISTORY_CAPACITY;
    for (uint32_t i = 0; i < g_history_header.record_count; ++i) {
        uint32_t slot = (oldest_slot + i) % POWER_HISTORY_CAPACITY;
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        power_history_record_t record;
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(&record, sizeof(record), 1, g_history_file) != 1) {
            xSemaphoreGive(g_history_mutex);
            ESP_LOGE(TAG, "Could not read persistent history record at slot %lu",
                     (unsigned long)slot);
            return ESP_FAIL;
        }
        if (!record_is_valid(&record)) {
            ESP_LOGW(TAG, "Ignoring corrupt history record at slot %lu",
                     (unsigned long)slot);
            continue;
        }
        if (record.sequence > after_sequence) {
            records[*record_count] = record;
            ++*record_count;
            if (*record_count == capacity) {
                break;
            }
        }
    }
    xSemaphoreGive(g_history_mutex);
    return ESP_OK;
}
