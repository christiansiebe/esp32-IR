#include "power_history.h"

#include <stdio.h>
#include <stdlib.h>
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
/* Format versions live in power_history.h so the host tests and the migration
 * helper agree on them. The header version is the current record version.
 * POWER_HISTORY_MOUNT_PATH/POWER_HISTORY_FILE also come from the header (they
 * are overridable there for the reboot-persistence host test). */
#define POWER_HISTORY_VERSION_MAX POWER_HISTORY_VERSION
#define POWER_HISTORY_PARTITION "storage"
#define POWER_HISTORY_QUEUE_LENGTH 16u
#define POWER_HISTORY_TASK_STACK 4096u
#define POWER_HISTORY_BATCH_RECORDS 5u
#define POWER_HISTORY_BATCH_TIMEOUT_MS 60000u

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

bool power_history_migrate_record(power_history_record_t *record)
{
    if (record == NULL || record->sequence == 0) {
        return false;
    }
    /* CRC stays strict across format versions: a mismatch is real corruption,
     * not a storage-layout change. The header stores the generation, so a
     * mismatch must not be masked by silently re-checksumming. */
    if (record->checksum != record_checksum(record)) {
        return false;
    }
    if (record->format_version > POWER_HISTORY_VERSION_MAX ||
        record->format_version < POWER_HISTORY_OLDEST_SUPPORTED_VERSION) {
        return false;
    }
    bool migrated = false;
    if (record->format_version == 1u) {
        /* v1 -> v2: the slot generation lived in `reserved`. v2 normalizes it
         * to 0; the value is not needed to read the record. */
        record->reserved = 0;
        record->format_version = POWER_HISTORY_VERSION;
        migrated = true;
    }
    if (record->format_version == 2u) {
        /* v2 -> v3: the new `reason` field did not exist yet, so every stored
         * minute is a measured one. */
        record->reason = POWER_HISTORY_REASON_NONE;
        record->reserved = 0;
        record->format_version = POWER_HISTORY_VERSION;
        migrated = true;
    }
    if (record->reason >= POWER_HISTORY_REASON_COUNT) {
        record->reason = POWER_HISTORY_REASON_UNKNOWN;
        migrated = true;
    }
    if (migrated) {
        /* The checksum covers the migrated fields, so it must be recomputed. */
        record->checksum = record_checksum(record);
    }
    return true;
}

static bool record_is_valid(const power_history_record_t *record)
{
    if (record == NULL || record->sequence == 0) {
        return false;
    }
    power_history_record_t migrated = *record;
    return power_history_migrate_record(&migrated);
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

void power_history_make_fresh_header(power_history_header_t *header)
{
    memset(header, 0, sizeof(*header));
    header->magic = POWER_HISTORY_MAGIC;
    header->version = POWER_HISTORY_VERSION;
    header->generation = 1;
    header->next_sequence = 1;
    header->checksum = header_checksum(header);
}

bool power_history_header_is_valid(const power_history_header_t *header)
{
    return header_is_valid(header);
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

static bool minute_is_stored(const power_history_header_t *header, int64_t ts)
{
    if (header->record_count == 0 || g_history_file == NULL) {
        return false;
    }
    uint32_t oldest_slot =
        (header->next_slot + POWER_HISTORY_CAPACITY - header->record_count) %
        POWER_HISTORY_CAPACITY;
    uint32_t lo = 0;
    uint32_t hi = header->record_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t slot = (oldest_slot + mid) % POWER_HISTORY_CAPACITY;
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        power_history_record_t record = {0};
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(&record, sizeof(record), 1, g_history_file) != 1 ||
            !record_is_valid(&record)) {
            /* Unreadable probe: treat as "not before ts" and keep searching. */
            hi = mid;
            continue;
        }
        if (record.interval_start_utc < ts) {
            lo = mid + 1u;
        } else if (record.interval_start_utc > ts) {
            hi = mid;
        } else {
            return true;
        }
    }
    return false;
}

static bool write_records(power_history_record_t *records, size_t record_count)
{
    power_history_header_t updated = g_history_header;
    for (size_t i = 0; i < record_count; ++i) {
        /* Dedupe by minute against what is already on disk: the live recorder and
         * the backfill sync both enqueue here, and a minute must exist only once.
         * Reusing the slot would corrupt the time ordering, so a duplicate is
         * simply dropped. */
        if (updated.record_count != 0 &&
            minute_is_stored(&updated, records[i].interval_start_utc)) {
            continue;
        }
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
        /* Format automatically when the partition is blank or its SPIFFS image
         * is unusable, so the persistent history works on first boot and after
         * a fresh flash without any manual erase. The stored data is
         * regenerable meter history, so a one-time reformat is acceptable. */
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&config);
    bool storage_available = true;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not mount persistent SPIFFS partition: %s; "
                      "continuing with RAM history",
                 esp_err_to_name(err));
        storage_available = false;
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
        power_history_make_fresh_header(&g_history_header);
        g_active_header_copy = 1;
    } else if (fseek(g_history_file, 0, SEEK_END) != 0 || ftell(g_history_file) == 0) {
        /* Empty file: write the fresh header into both copies. A previous bug
         * stored an all-zero (invalid) header here, so the next boot saw two
         * invalid copies and silently fell back to RAM, dropping every reboot. */
        power_history_make_fresh_header(&g_history_header);
        power_history_header_t previous = g_history_header;
        previous.generation = 0;
        previous.checksum = header_checksum(&previous);
        if (!write_header_copy(0, &previous) ||
            !write_header_copy(1, &g_history_header)) {
            ESP_LOGE(TAG, "Could not initialize persistent history metadata; continuing with RAM history");
            fclose(g_history_file);
            g_history_file = NULL;
            power_history_make_fresh_header(&g_history_header);
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
            /* Recover in place: create a fresh header instead of dropping to the
             * RAM-only fallback. The records may be gone, but history persists
             * across the next reboot again (and a diagnostic log explains why). */
            ESP_LOGW(TAG, "Persistent history metadata was invalid; reinitializing header");
            power_history_make_fresh_header(&g_history_header);
            power_history_header_t previous = g_history_header;
            previous.generation = 0;
            previous.checksum = header_checksum(&previous);
            if (!write_header_copy(0, &previous) ||
                !write_header_copy(1, &g_history_header)) {
                ESP_LOGE(TAG, "Could not write recovered history metadata; using RAM history");
                fclose(g_history_file);
                g_history_file = NULL;
                power_history_make_fresh_header(&g_history_header);
            }
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

esp_err_t power_history_merge(const power_history_record_t *records, size_t count,
                              size_t *merged_out)
{
    if (merged_out != NULL) {
        *merged_out = 0;
    }
    if (records == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_history_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The caller (the receiver's backfill session) only asks the sender for the
     * minutes its own presence map still misses, so merge does not repeat that
     * per-minute check: it validates each pulled record and enqueues it. The
     * store renumbers it, so the sender's and the receiver's independent
     * sequence counters never have to line up. */
    size_t merged = 0;
    for (size_t i = 0; i < count; ++i) {
        power_history_record_t record = records[i];
        if (!power_history_migrate_record(&record)) {
            continue;
        }
        if (!power_history_enqueue(&record)) {
            break;
        }
        ++merged;
    }
    if (merged_out != NULL) {
        *merged_out = merged;
    }
    return ESP_OK;
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
    /* Skip straight to the first record newer than after_sequence instead of
     * re-reading the whole ring (one fseek+fread per record) from the oldest
     * slot. Without this a large log made every history request block the HTTP
     * task for seconds. */
    uint32_t skip = 0;
    if (oldest_sequence != 0) {
        int64_t wanted = (int64_t)after_sequence + 1 - (int64_t)oldest_sequence;
        if (wanted > 0) {
            skip = (uint32_t)wanted;
        }
    }
    if (skip >= g_history_header.record_count) {
        xSemaphoreGive(g_history_mutex);
        return ESP_OK;
    }
    uint32_t first_slot = (oldest_slot + skip) % POWER_HISTORY_CAPACITY;
    for (uint32_t i = 0; i < g_history_header.record_count - skip; ++i) {
        uint32_t slot = (first_slot + i) % POWER_HISTORY_CAPACITY;
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
        (void)power_history_migrate_record(&record);
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

bool power_history_record_in_window(const power_history_record_t *record,
                                    int64_t from_utc, int64_t to_utc)
{
    if (record == NULL) {
        return false;
    }
    power_history_record_t migrated = *record;
    if (!power_history_migrate_record(&migrated)) {
        return false;
    }
    return migrated.interval_start_utc >= from_utc &&
           migrated.interval_start_utc < to_utc;
}

/* Binary search the ring for the first index whose record interval is >= ts.
 * Records are appended oldest-first, so the ring (walked from oldest_slot) is
 * sorted by interval_start_utc. Returns the index in [0, count]. */
static uint32_t find_first_index_at_or_after(uint32_t oldest_slot,
                                             uint32_t count, int64_t ts)
{
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t slot = (oldest_slot + mid) % POWER_HISTORY_CAPACITY;
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        power_history_record_t record = {0};
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(&record, sizeof(record), 1, g_history_file) != 1 ||
            !record_is_valid(&record)) {
            /* Treat an unreadable probe as "not before ts" so the search moves
             * on; the linear scan below re-reads and skips it anyway. */
            hi = mid;
            continue;
        }
        if (record.interval_start_utc < ts) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static esp_err_t read_window_ram(int64_t from_utc, int64_t to_utc,
                                 uint32_t after_sequence, uint32_t max_records,
                                 power_history_record_t *records,
                                 size_t *record_count, bool *more)
{
    for (size_t i = 0; i < g_history_ram_count; ++i) {
        const power_history_record_t *record = &g_history_ram_records[i];
        if (record->sequence <= after_sequence) {
            continue;
        }
        if (!power_history_record_in_window(record, from_utc, to_utc)) {
            continue;
        }
        if (*record_count == max_records) {
            *more = true;
            return ESP_OK;
        }
        records[(*record_count)++] = *record;
    }
    return ESP_OK;
}

esp_err_t power_history_read_window(int64_t from_utc, int64_t to_utc,
                                    uint32_t after_sequence, uint32_t max_records,
                                    power_history_record_t *records, size_t capacity,
                                    size_t *record_count, power_history_status_t *status,
                                    bool *more)
{
    if (records == NULL || capacity == 0 || record_count == NULL ||
        status == NULL || more == NULL || to_utc <= from_utc) {
        return ESP_ERR_INVALID_ARG;
    }
    *record_count = 0;
    *more = false;
    if (max_records == 0 || max_records > capacity) {
        max_records = (uint32_t)capacity;
    }
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

    esp_err_t result = ESP_OK;
    if (g_history_file == NULL) {
        result = read_window_ram(from_utc, to_utc, after_sequence, max_records,
                                 records, record_count, more);
        xSemaphoreGive(g_history_mutex);
        return result;
    }
    if (g_history_header.record_count == 0) {
        xSemaphoreGive(g_history_mutex);
        return ESP_OK;
    }

    uint32_t oldest_slot =
        (g_history_header.next_slot + POWER_HISTORY_CAPACITY -
         g_history_header.record_count) % POWER_HISTORY_CAPACITY;
    uint32_t index = find_first_index_at_or_after(oldest_slot,
                                                  g_history_header.record_count,
                                                  from_utc);
    /* Block-read forward from the first in-window record. Stop as soon as the
     * window's end is reached (records are time-ordered, so nothing later can
     * match) or one extra match proves further pages exist. */
    power_history_record_t block[64];
    uint32_t remaining = g_history_header.record_count - index;
    while (remaining != 0) {
        uint32_t slot = (oldest_slot + index) % POWER_HISTORY_CAPACITY;
        uint32_t chunk = remaining < 64 ? remaining : 64;
        if (slot + chunk > POWER_HISTORY_CAPACITY) {
            chunk = POWER_HISTORY_CAPACITY - slot;
        }
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(block, sizeof(block[0]), chunk, g_history_file) != chunk) {
            xSemaphoreGive(g_history_mutex);
            ESP_LOGE(TAG, "Could not read history window at slot %lu",
                     (unsigned long)slot);
            return ESP_FAIL;
        }
        for (uint32_t i = 0; i < chunk; ++i) {
            if (!record_is_valid(&block[i])) {
                continue;
            }
            power_history_record_t record = block[i];
            (void)power_history_migrate_record(&record);
            if (record.sequence <= after_sequence) {
                continue;
            }
            if (record.interval_start_utc >= to_utc) {
                xSemaphoreGive(g_history_mutex);
                return result;
            }
            if (*record_count == max_records) {
                *more = true;
                xSemaphoreGive(g_history_mutex);
                return result;
            }
            records[(*record_count)++] = record;
        }
        remaining -= chunk;
        index += chunk;
    }
    xSemaphoreGive(g_history_mutex);
    return result;
}

esp_err_t power_history_read_after(int64_t after_utc, uint32_t max_records,
                                   power_history_record_t *records, size_t capacity,
                                   size_t *record_count, power_history_status_t *status,
                                   bool *more)
{
    if (records == NULL || capacity == 0 || record_count == NULL ||
        status == NULL || more == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *record_count = 0;
    *more = false;
    if (max_records == 0 || max_records > capacity) {
        max_records = (uint32_t)capacity;
    }
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

    esp_err_t result = ESP_OK;
    if (g_history_file == NULL) {
        for (size_t i = 0; i < g_history_ram_count; ++i) {
            const power_history_record_t *record = &g_history_ram_records[i];
            if (record->interval_start_utc <= after_utc ||
                !record_is_valid(record)) {
                continue;
            }
            if (*record_count == max_records) {
                *more = true;
                break;
            }
            records[(*record_count)++] = *record;
        }
        xSemaphoreGive(g_history_mutex);
        return result;
    }
    if (g_history_header.record_count == 0) {
        xSemaphoreGive(g_history_mutex);
        return ESP_OK;
    }

    uint32_t oldest_slot =
        (g_history_header.next_slot + POWER_HISTORY_CAPACITY -
         g_history_header.record_count) % POWER_HISTORY_CAPACITY;
    /* The ring, walked from the oldest slot, is time-ordered, so skip straight to
     * the first record after the cursor. This also reaches the very oldest record
     * (binary search returns index 0 when after_utc is before it), which a
     * window read with from_utc=0 cannot. */
    uint32_t index = find_first_index_at_or_after(oldest_slot,
                                                  g_history_header.record_count,
                                                  after_utc + 1);
    power_history_record_t block[64];
    uint32_t remaining = g_history_header.record_count - index;
    while (remaining != 0) {
        uint32_t slot = (oldest_slot + index) % POWER_HISTORY_CAPACITY;
        uint32_t chunk = remaining < 64 ? remaining : 64;
        if (slot + chunk > POWER_HISTORY_CAPACITY) {
            chunk = POWER_HISTORY_CAPACITY - slot;
        }
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(block, sizeof(block[0]), chunk, g_history_file) != chunk) {
            xSemaphoreGive(g_history_mutex);
            ESP_LOGE(TAG, "Could not read history page at slot %lu",
                     (unsigned long)slot);
            return ESP_FAIL;
        }
        for (uint32_t i = 0; i < chunk; ++i) {
            if (!record_is_valid(&block[i])) {
                continue;
            }
            power_history_record_t record = block[i];
            (void)power_history_migrate_record(&record);
            if (record.interval_start_utc <= after_utc) {
                continue;
            }
            if (*record_count == max_records) {
                *more = true;
                xSemaphoreGive(g_history_mutex);
                return result;
            }
            records[(*record_count)++] = record;
        }
        remaining -= chunk;
        index += chunk;
    }
    xSemaphoreGive(g_history_mutex);
    return result;
}

bool power_history_has_minute(int64_t interval_start_utc)
{
    if (!g_history_ready ||
        xSemaphoreTake(g_history_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool found = false;
    if (g_history_file == NULL) {
        for (size_t i = 0; i < g_history_ram_count; ++i) {
            if (g_history_ram_records[i].interval_start_utc == interval_start_utc) {
                found = true;
                break;
            }
        }
    } else {
        found = minute_is_stored(&g_history_header, interval_start_utc);
    }
    xSemaphoreGive(g_history_mutex);
    return found;
}

const char *power_history_reason_token(uint8_t reason)
{
    switch (reason) {
    case POWER_HISTORY_REASON_NONE: return "none";
    case POWER_HISTORY_REASON_UNKNOWN: return "unknown";
    case POWER_HISTORY_REASON_OTA: return "ota";
    case POWER_HISTORY_REASON_CRASH: return "crash";
    case POWER_HISTORY_REASON_BROWNOUT: return "brownout";
    case POWER_HISTORY_REASON_POWERON: return "poweron";
    case POWER_HISTORY_REASON_RESTART: return "restart";
    default: return "unknown";
    }
}

uint8_t power_history_reason_from_reset(int reset_reason, bool deliberate_ota)
{
    if (deliberate_ota) {
        return POWER_HISTORY_REASON_OTA;
    }
    switch (reset_reason) {
    /* esp_reset_reason_t numbering. 1 = power-on, 3 = software restart,
     * 8 = deep sleep. A power-on reset with no prior "seen" minute is handled
     * by the caller (it is a cold boot, not a gap). */
    case 1: return POWER_HISTORY_REASON_POWERON;
    case 4: /* panic */
    case 5: /* interrupt watchdog */
    case 6: /* task watchdog */
    case 7: /* other watchdog */
        return POWER_HISTORY_REASON_CRASH;
    case 9: return POWER_HISTORY_REASON_BROWNOUT;
    case 3: /* software restart */
    case 8: /* deep sleep */
        return POWER_HISTORY_REASON_RESTART;
    default: return POWER_HISTORY_REASON_UNKNOWN;
    }
}

esp_err_t power_history_summarize_window(int64_t from_utc, int64_t to_utc,
                                         power_history_gap_summary_t *summary)
{
    if (summary == NULL || to_utc <= from_utc) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(summary, 0, sizeof(*summary));
    from_utc -= from_utc % 60;
    to_utc -= to_utc % 60;
    summary->total = (uint32_t)((to_utc - from_utc) / 60);
    if (!g_history_ready) {
        summary->missing = summary->total;
        return ESP_ERR_INVALID_STATE;
    }
    /* Page through the window in time order; both backends return the records
     * sorted by minute, so a walk of the returned pages sees every stored
     * minute in [from_utc, to_utc) exactly once. */
    power_history_record_t *records =
        calloc(POWER_HISTORY_PAGE_SIZE, sizeof(*records));
    if (records == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int64_t after = from_utc - 1;
    for (;;) {
        power_history_status_t status;
        bool more = false;
        size_t count = 0;
        esp_err_t err = power_history_read_after(after, POWER_HISTORY_PAGE_SIZE,
                                                 records, POWER_HISTORY_PAGE_SIZE,
                                                 &count, &status, &more);
        if (err != ESP_OK) {
            free(records);
            return err;
        }
        bool stop = false;
        for (size_t i = 0; i < count; ++i) {
            int64_t ts = records[i].interval_start_utc;
            if (ts < from_utc) {
                continue;
            }
            if (ts >= to_utc) {
                stop = true;
                break;
            }
            ++summary->present;
            if (records[i].reason == POWER_HISTORY_REASON_NONE) {
                ++summary->measured;
            } else {
                ++summary->expected;
            }
            uint8_t reason = records[i].reason;
            if (reason < POWER_HISTORY_REASON_COUNT) {
                ++summary->reasons[reason];
            }
            after = ts;
        }
        if (stop || !more || count == 0) {
            break;
        }
    }
    free(records);
    if (summary->present <= summary->total) {
        summary->missing = summary->total - summary->present;
    }
    return ESP_OK;
}

void power_history_aggregate_init(int64_t window_start_utc, int64_t bucket_seconds,
                                  double *sum_w, uint32_t *count,
                                  int64_t *bucket_start_ms, size_t bucket_count)
{
    for (size_t i = 0; i < bucket_count; ++i) {
        sum_w[i] = 0.0;
        count[i] = 0;
        bucket_start_ms[i] = window_start_utc * 1000LL +
                             (int64_t)i * bucket_seconds * 1000LL;
    }
}

esp_err_t power_history_aggregate_core(int64_t window_start_utc,
                                       int64_t bucket_seconds,
                                       const power_history_record_t *records,
                                       size_t record_count, double *sum_w,
                                       uint32_t *count, size_t bucket_count)
{
    if (sum_w == NULL || count == NULL || bucket_count == 0 ||
        bucket_seconds <= 0 || (record_count != 0 && records == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < record_count; ++i) {
        if (!record_is_valid(&records[i])) {
            continue;
        }
        /* Work on a migrated copy so older-format records are aggregated too
         * (a version bump must not silently drop the stored history). */
        power_history_record_t record = records[i];
        (void)power_history_migrate_record(&record);
        int64_t ts = record.interval_start_utc;
        if (ts < window_start_utc) {
            continue;
        }
        int64_t idx = (ts - window_start_utc) / bucket_seconds;
        if (idx < 0 || idx >= (int64_t)bucket_count) {
            continue;
        }
        sum_w[idx] += (double)record.average_milliwatts / 1000.0;
        ++count[idx];
    }
    return ESP_OK;
}

esp_err_t power_history_aggregate(int64_t window_start_utc, int64_t bucket_seconds,
                                  double *sum_w, uint32_t *count, int64_t *bucket_start_ms,
                                  size_t bucket_count, power_history_status_t *status)
{
    if (sum_w == NULL || count == NULL || bucket_start_ms == NULL ||
        bucket_count == 0 || bucket_seconds <= 0 || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    power_history_aggregate_init(window_start_utc, bucket_seconds, sum_w, count,
                                 bucket_start_ms, bucket_count);
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

    if (g_history_file == NULL) {
        esp_err_t core_err = power_history_aggregate_core(
            window_start_utc, bucket_seconds, g_history_ram_records,
            g_history_ram_count, sum_w, count, bucket_count);
        xSemaphoreGive(g_history_mutex);
        return core_err;
    }

    uint32_t oldest_slot =
        (g_history_header.next_slot + POWER_HISTORY_CAPACITY -
         g_history_header.record_count) % POWER_HISTORY_CAPACITY;
    /* Walk the ring once but read in blocks: a large log made the per-record
     * fseek+fread dominate (tens of thousands of syscalls) and blocked the HTTP
     * task. A small block buffer keeps RAM flat and stays correct across the
     * ring wrap. */
    power_history_record_t block[64];
    uint32_t remaining = g_history_header.record_count;
    uint32_t index = 0;
    while (remaining != 0) {
        uint32_t slot = (oldest_slot + index) % POWER_HISTORY_CAPACITY;
        uint32_t chunk = remaining < 64 ? remaining : 64;
        if (slot + chunk > POWER_HISTORY_CAPACITY) {
            chunk = POWER_HISTORY_CAPACITY - slot;
        }
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(block, sizeof(block[0]), chunk, g_history_file) != chunk) {
            xSemaphoreGive(g_history_mutex);
            ESP_LOGE(TAG, "Could not read persistent history records at slot %lu",
                     (unsigned long)slot);
            return ESP_FAIL;
        }
        if (power_history_aggregate_core(window_start_utc, bucket_seconds, block,
                                         chunk, sum_w, count, bucket_count) != ESP_OK) {
            xSemaphoreGive(g_history_mutex);
            return ESP_FAIL;
        }
        remaining -= chunk;
        index += chunk;
    }
    xSemaphoreGive(g_history_mutex);
    return ESP_OK;
}

