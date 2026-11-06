#include "power_history.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "task_wdt_guard.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

#define POWER_HISTORY_MAGIC 0x50484C31u
/* Format versions live in power_history.h so the host tests and the migration
 * helper agree on them. The header version is the current record version.
 * POWER_HISTORY_MOUNT_PATH/POWER_HISTORY_FILE also come from the header (they
 * are overridable there for the reboot-persistence host test). */
#define POWER_HISTORY_VERSION_MAX POWER_HISTORY_VERSION
#define POWER_HISTORY_QUEUE_LENGTH 16u
#define POWER_HISTORY_TASK_STACK 4096u
#define POWER_HISTORY_BATCH_RECORDS 5u
#define POWER_HISTORY_BATCH_TIMEOUT_MS 60000u
/* Records moved per file-I/O round when an out-of-order (backfill) minute is
 * inserted into the sorted ring. Keeps the shift bounded without a full-store
 * buffer (a 29000-record ring is ~812 kB, far past the free heap). */
#define POWER_HISTORY_SHIFT_CHUNK 32u
/* Yield to lower-priority tasks (and the IDLE task that feeds the watchdog)
 * after this many shift chunks, so a long in-place ring insert cannot starve
 * the system while it holds the mutex. */
#define POWER_HISTORY_SHIFT_YIELD_CHUNKS 8u
/* Reads take the mutex with a bounded wait instead of portMAX_DELAY: while the
 * writer (or a CSV import) holds it for a long in-place ring insert, the HTTP
 * task must be able to give up and answer quickly rather than stalling until
 * the browser's own request timeout fires (the reported "AbortError"). */
/* Wait for the store lock. A reader must outlast one writer round: the writer
 * holds the mutex while it splices a backfill batch (shift_up_renumber moves up
 * to POWER_HISTORY_MAX_SPLICE_RECORDS records, each a fseek+fwrite). At 800 ms
 * every reader timed out under backfill load and the dashboard showed an empty
 * graph even though the ring was full. The bound still keeps the HTTP task from
 * blocking indefinitely. */
#define POWER_HISTORY_READ_LOCK_TIMEOUT_MS 2500u
/* The startup repair must never block the boot: a damaged/full SPIFFS store
 * used to make a single lseek hang for minutes, wedging app_main before the
 * web server started and boot-looping the device. Two independent safeguards:
 *
 *  1. The repair does NOT run inside power_history_init() anymore. It runs in
 *     its own low-priority background task that starts only after a delay
 *     (POWER_HISTORY_REPAIR_DELAY_MS) long enough for HTTP/OTA/telemetry to be
 *     up. The device boots and serves regardless of the store's state.
 *  2. The repair loops yield to the scheduler every
 *     POWER_HISTORY_REPAIR_YIELD_STEPS steps and the whole repair is bounded
 *     by a wall-clock budget. On budget expiry it is abandoned best-effort
 *     (the ring is left as it was; a later boot retries). */
#define POWER_HISTORY_REPAIR_YIELD_STEPS 2048u
#define POWER_HISTORY_REPAIR_BUDGET_MS 15000u
/* Heap slice the repair leaves untouched for the HTTP/OTA/Wi-Fi paths that
 * allocate while it runs. The repair buffer is clamped to
 * (largest free block - this), so it can never starve a concurrent allocation. */
#define POWER_HISTORY_REPAIR_HEAP_HEADROOM 16384u
/* Smallest repair buffer worth taking. Below this the repair gives up and keeps
 * the ring as-is (a later boot retries), rather than looping on tiny sizes. */
#define POWER_HISTORY_REPAIR_MIN_RECORDS 64u
/* Delay before the background repair task touches the store, giving the boot
 * (Wi-Fi, HTTP server, OTA) time to come up first. */
#define POWER_HISTORY_REPAIR_DELAY_MS 10000u
/* Matches the writer's stack: the repair does the same SPIFFS fseek/fread plus
 * heap allocations and logging. The task deletes itself when done, so the
 * larger stack costs nothing after boot; a too-small stack would abort on the
 * canary check, which is the failure this task must never cause. */
#define POWER_HISTORY_REPAIR_STACK 4096u

static const char *TAG = "power_history";
static FILE *g_history_file;
static power_history_header_t g_history_header;
static uint8_t g_active_header_copy;
static QueueHandle_t g_history_queue;
static SemaphoreHandle_t g_history_mutex;
static uint32_t g_queue_drops;
static uint32_t g_write_errors;
/* Write-amplification counters (see power_history_status_t). Written by the
 * history writer under g_history_mutex, read via get_status under the same
 * mutex, so no separate lock is needed. */
static uint32_t g_splice_batches;
static uint32_t g_splice_records;
static uint32_t g_splice_skipped;
static uint32_t g_storage_used_bytes;
static uint32_t g_storage_total_bytes;
static bool g_history_ready;
static bool g_history_ram_fallback;
static power_history_record_t g_history_ram_records[POWER_HISTORY_PAGE_SIZE];
static size_t g_history_ram_count;
static portMUX_TYPE g_stats_lock = portMUX_INITIALIZER_UNLOCKED;

/* Optional observer for newly stored records (see power_history.h). Set once at
 * boot, read from the writer while it holds g_history_mutex. */
static power_history_store_observer_t g_store_observer;
static void *g_store_observer_ctx;

void power_history_set_store_observer(power_history_store_observer_t observer,
                                      void *ctx)
{
    g_store_observer = observer;
    g_store_observer_ctx = ctx;
}

/* Notify the observer that one minute was actually persisted. The writer holds
 * the store lock; the observer must not block. */
static void notify_store_observer(const power_history_record_t *record)
{
    if (g_store_observer != NULL) {
        g_store_observer(g_store_observer_ctx, record);
    }
}

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

/* File offset of ring index `index` (0 = oldest) for an explicit ring geometry.
 * The store keeps the ring sorted by interval_start_utc, so index order is time
 * order. */
static long offset_for_slot(uint32_t oldest_slot, uint32_t index)
{
    uint32_t slot = (oldest_slot + index) % POWER_HISTORY_CAPACITY;
    return (long)(2u * sizeof(power_history_header_t) +
                  slot * sizeof(power_history_record_t));
}

static uint32_t oldest_slot_of(uint32_t next_slot, uint32_t count)
{
    return (next_slot + POWER_HISTORY_CAPACITY - count) % POWER_HISTORY_CAPACITY;
}

static bool read_record_idx(uint32_t oldest_slot, uint32_t index,
                            power_history_record_t *record)
{
    return fseek(g_history_file, offset_for_slot(oldest_slot, index), SEEK_SET) == 0 &&
           fread(record, sizeof(*record), 1, g_history_file) == 1;
}

static bool write_record_idx(uint32_t oldest_slot, uint32_t index,
                             const power_history_record_t *record)
{
    return fseek(g_history_file, offset_for_slot(oldest_slot, index), SEEK_SET) == 0 &&
           fwrite(record, sizeof(*record), 1, g_history_file) == 1;
}

/* Batched ring I/O: read/write `count` consecutive records starting at ring
 * index `start`. The slots of a contiguous index range are contiguous modulo
 * POWER_HISTORY_CAPACITY, so this needs at most two stdio runs (one split at the
 * wrap) instead of one fseek+fread/fwrite per record.
 *
 * This matters for the lock hold time: every fseek discards the stdio buffer,
 * so a per-record loop turns each 32-byte record into its own SPIFFS page
 * operation. A deep backfill splice of ~350 records then held g_history_mutex
 * for ~4 s, which made every dashboard read time out (the graph showed an empty
 * store). Reading a whole chunk in one call cuts the syscalls by the chunk
 * factor, so the lock is free again between splices. */
static bool read_range_idx(uint32_t oldest_slot, uint32_t start, uint32_t count,
                           power_history_record_t *buf)
{
    uint32_t done = 0;
    while (done < count) {
        uint32_t slot = (oldest_slot + start + done) % POWER_HISTORY_CAPACITY;
        uint32_t run = count - done;
        if (slot + run > POWER_HISTORY_CAPACITY) {
            run = POWER_HISTORY_CAPACITY - slot;
        }
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(buf + done, sizeof(buf[0]), run, g_history_file) != run) {
            return false;
        }
        done += run;
    }
    return true;
}

static bool write_range_idx(uint32_t oldest_slot, uint32_t start, uint32_t count,
                            const power_history_record_t *buf)
{
    uint32_t done = 0;
    while (done < count) {
        uint32_t slot = (oldest_slot + start + done) % POWER_HISTORY_CAPACITY;
        uint32_t run = count - done;
        if (slot + run > POWER_HISTORY_CAPACITY) {
            run = POWER_HISTORY_CAPACITY - slot;
        }
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fwrite(buf + done, sizeof(buf[0]), run, g_history_file) != run) {
            return false;
        }
        done += run;
    }
    return true;
}

/* First index in [0, count] whose minute is >= ts. The ring is time-sorted, so
 * this is the insertion point for a record of that minute. */
static uint32_t find_first_at_or_after_time(uint32_t oldest_slot, uint32_t count,
                                            int64_t ts)
{
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        power_history_record_t record = {0};
        if (!read_record_idx(oldest_slot, mid, &record) || !record_is_valid(&record)) {
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

/* Same as minute_is_stored() but for an explicit geometry, so a batch insert can
 * dedupe against the records it has already placed in this pass. */
static bool minute_is_stored_geom(uint32_t next_slot, uint32_t count, int64_t ts)
{
    if (count == 0) {
        return false;
    }
    uint32_t oldest_slot = oldest_slot_of(next_slot, count);
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        power_history_record_t record = {0};
        if (!read_record_idx(oldest_slot, mid, &record) || !record_is_valid(&record)) {
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

/* Shift indices [at, count) up by one slot (index i -> i+1) so a new record can
 * be written at `at`, and bump each moved record's sequence by one. Sequences
 * are kept contiguous (sequence == oldest_sequence + index) and therefore
 * monotonic in ring order, which the after_sequence paging (CSV/dashboard)
 * relies on. Chunked so the working set stays bounded; the topmost write (index
 * `count`) lands in the slot the oldest record occupied, evicting it when the
 * ring is full. */
static bool shift_up_renumber(uint32_t oldest_slot, uint32_t count, uint32_t at)
{
    power_history_record_t buf[POWER_HISTORY_SHIFT_CHUNK];
    uint32_t index = count;
    uint32_t chunks_done = 0;
    while (index > at) {
        uint32_t chunk = index - at;
        if (chunk > POWER_HISTORY_SHIFT_CHUNK) {
            chunk = POWER_HISTORY_SHIFT_CHUNK;
        }
        uint32_t start = index - chunk; /* move [start, index) -> [start+1, index+1) */
        if (!read_range_idx(oldest_slot, start, chunk, buf)) {
            return false;
        }
        for (uint32_t i = 0; i < chunk; ++i) {
            ++buf[i].sequence;
            if (buf[i].sequence == 0) {
                buf[i].sequence = 1;
            }
            buf[i].checksum = record_checksum(&buf[i]);
        }
        if (!write_range_idx(oldest_slot, start + 1u, chunk, buf)) {
            return false;
        }
        index = start;
        if (++chunks_done % POWER_HISTORY_SHIFT_YIELD_CHUNKS == 0) {
            /* Let the IDLE task (watchdog) and any equal/higher-priority waiter
             * run between file-I/O rounds. The mutex is still held, so a reader
             * waits at most one chunk round before its bounded take succeeds. */
            vTaskDelay(1);
        }
    }
    return true;
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

/* Report *where* and *why* a persistence attempt failed. The VFS sets errno on
 * the SPIFFS path, but the caller only ever saw "could not persist", so a
 * GC-starved, nearly-full SPIFFS (writes fail with ENOSPC while reads still
 * work) was indistinguishable from a plain I/O fault. Log the errno plus the
 * partition fill so the two can be told apart from the device log alone. */
static void log_persist_failure(const char *stage)
{
    int err = errno;
#ifdef ESP_PLATFORM
    size_t total = 0;
    size_t used = 0;
    esp_err_t info = esp_spiffs_info(POWER_HISTORY_PARTITION, &total, &used);
    ESP_LOGE(TAG, "History persist failed at %s: errno=%d (%s), spiffs=%s used=%u/%u",
             stage, err, strerror(err),
             info == ESP_OK ? "ok" : esp_err_to_name(info),
             (unsigned)used, (unsigned)total);
#else
    ESP_LOGE(TAG, "History persist failed at %s: errno=%d (%s)", stage, err,
             strerror(err));
    (void)stage; /* ESP_LOGE is a no-op on the host */
#endif
}

/* Append/insert a batch of validated records into the ring, keeping it sorted by
 * interval_start_utc. The live recorder appends (newest minute = tail) so that is
 * a cheap write at next_slot; a backfill merge carries an OLDER minute and is
 * spliced into its sorted position, shifting the newer records up. Keeping the
 * ring time-sorted is what makes the binary-search dedupe (minute_is_stored) and
 * the time-ordered window/CSV reads correct - the previous append-only code left
 * the ring unsorted after a merge, so existing minutes were no longer found and
 * got written again (the reported duplicates). */
static bool write_records(power_history_record_t *records, size_t record_count)
{
    power_history_header_t updated = g_history_header;
    /* Sort the batch by minute so a mixed live+merge batch inserts in order. */
    for (size_t i = 1; i < record_count; ++i) {
        power_history_record_t key = records[i];
        size_t j = i;
        while (j > 0 && records[j - 1].interval_start_utc > key.interval_start_utc) {
            records[j] = records[j - 1];
            --j;
        }
        records[j] = key;
    }

    for (size_t i = 0; i < record_count; ++i) {
        /* A minute before the valid-clock bound (0 / 1970 without a wall clock)
         * is not a real measurement and would sort before every stored minute;
         * drop it so the time-ordered ring stays consistent. */
        if (records[i].interval_start_utc < POWER_HISTORY_MIN_VALID_UTC) {
            continue;
        }
        /* Dedupe by minute against what is already stored (including records this
         * batch has already placed): a minute must exist only once. */
        if (minute_is_stored_geom(updated.next_slot, updated.record_count,
                                  records[i].interval_start_utc)) {
            continue;
        }
        uint32_t oldest_slot = oldest_slot_of(updated.next_slot, updated.record_count);
        /* Contiguous sequence numbering: the oldest stored record is
         * next_sequence - record_count, so index `at` gets that plus `at`. */
        uint32_t oldest_sequence = updated.next_sequence - updated.record_count;
        uint32_t at = find_first_at_or_after_time(oldest_slot, updated.record_count,
                                                  records[i].interval_start_utc);
        records[i].sequence = oldest_sequence + at;
        if (records[i].sequence == 0) {
            records[i].sequence = 1;
        }
        records[i].format_version = POWER_HISTORY_VERSION;
        records[i].reserved = 0;
        records[i].checksum = record_checksum(&records[i]);

        if (at < updated.record_count) {
            /* Bound the splice: shifting more than POWER_HISTORY_MAX_SPLICE_
             * RECORDS newer records costs O(ring) SPIFFS I/O under the mutex and
             * is what let the receiver's backfill livelock. A minute that far
             * below the tail is a deep gap the ring cannot take in order; drop
             * it so the store stays sorted and the writer keeps up. The record
             * is counted as merged by the caller, so the sync cursor still moves
             * past it and the session does not re-pull it forever. */
            if (updated.record_count - at > POWER_HISTORY_MAX_SPLICE_RECORDS) {
                ++g_splice_skipped;
                continue;
            }
            /* Splice in: shift the newer records up and renumber them, so the
             * ring stays sorted by minute AND by sequence (the after_sequence
             * cursor needs the latter). */
            if (!shift_up_renumber(oldest_slot, updated.record_count, at)) {
                log_persist_failure("shift");
                return false;
            }
            /* Count the splice: (record_count - at) records were rewritten just
             * to make room for one older minute. A backfill that keeps merging
             * minutes below the local tail drives this to the full ring per
             * minute - the write-amplification that starves SPIFFS garbage
             * collection on an almost-full partition. */
            ++g_splice_batches;
            g_splice_records += updated.record_count - at;
            if (!write_record_idx(oldest_slot, at, &records[i])) {
                log_persist_failure("splice-write");
                return false;
            }
        } else {
            long offset = (long)(2u * sizeof(power_history_header_t) +
                                 updated.next_slot * sizeof(records[i]));
            if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
                fwrite(&records[i], sizeof(records[i]), 1, g_history_file) != 1) {
                log_persist_failure("append-write");
                return false;
            }
        }
        notify_store_observer(&records[i]);
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
        log_persist_failure("flush");
        return false;
    }
    ++updated.generation;
    updated.checksum = header_checksum(&updated);

    uint8_t next_copy = (uint8_t)(1u - g_active_header_copy);
    if (!write_header_copy(next_copy, &updated)) {
        log_persist_failure("header");
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
    /* Prove liveness to the task watchdog: a wedged writer (mutex deadlock, a
     * SPIFFS call that never returns) must reboot the device instead of
     * silently stopping the minute history. */
    task_wdt_guard_subscribe(TASK_WDT_NAME_POWER_HISTORY);
    while (1) {
        /* Bounded wait, not portMAX_DELAY: an idle writer still wakes to feed
         * the watchdog, so a healthy-but-idle worker is never mistaken for a
         * stall. Still a blocking wait (no busy loop). */
        if (xQueueReceive(g_history_queue, &records[0],
                          pdMS_TO_TICKS(POWER_HISTORY_BATCH_TIMEOUT_MS)) != pdTRUE) {
            task_wdt_guard_feed();
            continue;
        }
        size_t record_count = 1;
        while (record_count < POWER_HISTORY_BATCH_RECORDS &&
               xQueueReceive(g_history_queue, &records[record_count],
                             pdMS_TO_TICKS(POWER_HISTORY_BATCH_TIMEOUT_MS)) == pdTRUE) {
            ++record_count;
        }
        task_wdt_guard_feed();
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
                notify_store_observer(&records[i]);
            }
        }
        xSemaphoreGive(g_history_mutex);
        task_wdt_guard_feed();
    }
}

/* Ordering key for the repair sort: minute first, then the stored sequence so
 * duplicate minutes keep a deterministic order (the lower sequence, i.e. the
 * copy written earlier, survives the dedup). */
static bool record_precedes(const power_history_record_t *a,
                            const power_history_record_t *b)
{
    if (a->interval_start_utc != b->interval_start_utc) {
        return a->interval_start_utc < b->interval_start_utc;
    }
    return a->sequence < b->sequence;
}

/* Paces a long on-disk repair loop: yields to the scheduler every
 * POWER_HISTORY_REPAIR_YIELD_STEPS steps so the IDLE task (and with it the task
 * watchdog) keeps running, and reports when the wall-clock budget is spent so
 * the caller can abandon the repair instead of blocking the boot forever. */
typedef struct {
    uint32_t steps;
    uint32_t budget_ms;
    int64_t deadline_us; /* 0 = no clock available (host build): no budget */
    bool expired;
} repair_pacer_t;

static int64_t repair_now_us(void)
{
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return 0; /* host build has no esp_timer; the pacer then never expires */
#endif
}

/* Records the repair may hold in one RAM buffer for a given largest free block:
 * the compile-time upper bound, clamped to (largest block - headroom). A fixed
 * buffer made the repair give up when the free heap had shrunk below it (the
 * ring then stayed damaged and every new minute failed), so the size follows the
 * actual heap instead of being assumed. Pure and overflow-safe, so the host test
 * can drive it with any heap size. */
uint32_t power_history_repair_keep_limit_for(size_t largest_free_block,
                                             uint32_t upper)
{
    if (largest_free_block <= POWER_HISTORY_REPAIR_HEAP_HEADROOM) {
        return 0;
    }
    size_t usable = largest_free_block - POWER_HISTORY_REPAIR_HEAP_HEADROOM;
    size_t fits = usable / sizeof(power_history_record_t);
    if (fits > 0xFFFFFFFFu) {
        fits = 0xFFFFFFFFu; /* clamp before the narrowing cast */
    }
    uint32_t limit = (uint32_t)fits;
    return limit < upper ? limit : upper;
}

static uint32_t repair_keep_limit(void)
{
    return power_history_repair_keep_limit_for(
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (uint32_t)POWER_HISTORY_REPAIR_RAM_LIMIT);
}

/* Allocate the repair buffer for at most `*records_wanted` records, halving the
 * request until the allocation fits (heap fragmentation can make a size fail
 * that a smaller one accepts). On success the actually reserved record count is
 * written back through `*records_wanted`. Returns NULL when even the minimum
 * does not fit, so the caller keeps the ring as-is instead of aborting. */
static power_history_record_t *repair_alloc(uint32_t *records_wanted)
{
    uint32_t want = *records_wanted;
    uint32_t limit = repair_keep_limit();
    if (want > limit) {
        want = limit;
    }
    /* Halve only down to the caller's own size when that is already small (a
     * short ring is legitimately small); otherwise down to the minimum worth
     * allocating. */
    uint32_t floor = want < POWER_HISTORY_REPAIR_MIN_RECORDS
                         ? want
                         : (uint32_t)POWER_HISTORY_REPAIR_MIN_RECORDS;
    while (want >= floor && want != 0) {
        power_history_record_t *buf =
            heap_caps_malloc((size_t)want * sizeof(*buf), MALLOC_CAP_8BIT);
        if (buf != NULL) {
            *records_wanted = want;
            return buf;
        }
        if (want == floor) {
            break;
        }
        want /= 2u;
        if (want < floor) {
            want = floor;
        }
    }
    return NULL;
}

static void repair_pacer_begin(repair_pacer_t *pacer)
{
    pacer->steps = 0;
    pacer->budget_ms = POWER_HISTORY_REPAIR_BUDGET_MS;
    pacer->expired = false;
    int64_t now = repair_now_us();
    pacer->deadline_us = (now != 0)
                             ? now + (int64_t)pacer->budget_ms * 1000
                             : 0;
}

/* Account one unit of work. Returns false once the budget is spent; the caller
 * must then stop and report the repair as incomplete. */
static bool repair_pacer_step(repair_pacer_t *pacer)
{
    if (++pacer->steps % POWER_HISTORY_REPAIR_YIELD_STEPS == 0) {
        /* Let the IDLE task feed the task watchdog. Delay(1) blocks this task
         * for one tick (10 ms at 100 Hz), long enough for IDLE to run. */
        vTaskDelay(1);
        if (pacer->deadline_us != 0 && repair_now_us() >= pacer->deadline_us) {
            pacer->expired = true;
            return false;
        }
    }
    return true;
}

/* Commit a repaired ring: flush the records and publish a new header that keeps
 * `next_sequence` (so an after_sequence cursor stays valid) while the oldest
 * sequence moves up to `next_sequence - unique`. */
static bool finish_repair(uint32_t oldest_slot, uint32_t unique)
{
    if (fflush(g_history_file) != 0 || fsync(fileno(g_history_file)) != 0) {
        return false;
    }
    uint32_t next_sequence = g_history_header.next_sequence;
    uint32_t oldest_sequence = next_sequence - unique;
    if (oldest_sequence == 0) {
        oldest_sequence = 1;
    }
    power_history_header_t updated = g_history_header;
    updated.record_count = unique;
    updated.next_sequence = next_sequence;
    updated.next_slot = (oldest_slot + unique) % POWER_HISTORY_CAPACITY;
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

static bool repair_ring_if_needed(void);
/* Defined with the store-recovery helpers below; called by the background
 * repair task to bring a RAM-only store back to a persistent one. */
static bool recover_store_locked(void);

/* Heal SPIFFS' own bookkeeping before touching the ring. A hard reset mid-write
 * (this board's normal failure mode) can leave the file system's index
 * inconsistent: reads then fail with ENOENT/EIO on a fixed set of slots even
 * though the partition has plenty of free space. The ring keeps growing into
 * that region and every write that reaches it fails, which is exactly the
 * "write errors climb, ring stalls at the same record count" fault. The
 * SPIFFS check repairs the index in place and keeps the data (unlike a format),
 * so it is the right first step. Best-effort: a failure only means the ring
 * repair below still runs, and a later boot retries. */
static void heal_spiffs_consistency(void)
{
    esp_err_t err = esp_spiffs_check(POWER_HISTORY_PARTITION);
    if (err == ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "SPIFFS consistency check failed: %s", esp_err_to_name(err));
}

/* Repair a ring that an older firmware left time-unsorted and/or holding
 * duplicate minutes (backfill merges used to append out of order, and the
 * binary-search dedupe then failed to see the minutes already stored). The ring
 * is sorted by (minute, sequence), duplicate minutes are dropped (the earliest
 * copy survives) and sequence is renumbered so sequence == oldest + index. The
 * after_sequence cursor stays valid because next_sequence is preserved.
 *
 * Two paths, so a full ring is repaired too:
 *  - a small store is sorted in one RAM buffer (fast, the common case);
 *  - a store too large for one buffer keeps only the most recent records that
 *    fit one RAM buffer (the earlier in-place disk sort is gone: a damaged/full
 *    SPIFFS store can make a single lseek hang for minutes, which used to wedge
 *    the boot). The sender backfill re-imports the dropped minutes.
 * Returns true when the ring is sorted and duplicate-free afterwards. */
esp_err_t power_history_repair_ring(void)
{
    if (g_history_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Could not lock history store for ring repair");
        return ESP_ERR_TIMEOUT;
    }
    if (g_history_file == NULL) {
        /* The partition/file never opened, so the store is RAM-only. Bring it
         * back to a persistent one first (format as the last resort); the ring
         * repair below then runs against the fresh file. */
        (void)recover_store_locked();
    } else {
        heal_spiffs_consistency();
    }
    bool ok = repair_ring_if_needed();
    xSemaphoreGive(g_history_mutex);
    (void)ok;
    return ESP_OK;
}

static bool repair_ring_if_needed(void)
{
    if (g_history_file == NULL || g_history_header.record_count < 2) {
        return true;
    }
    uint32_t count = g_history_header.record_count;
    uint32_t oldest_slot = oldest_slot_of(g_history_header.next_slot, count);
    /* Paces every loop below (the damage scan is already O(count) SPIFFS
     * reads) so the boot cannot be starved of the IDLE task. */
    repair_pacer_t pacer;
    repair_pacer_begin(&pacer);

    /* Streaming damage check: O(1) RAM, so it also covers a full ring. */
    bool damaged = false;
    int64_t prev = INT64_MIN;
    bool have_prev = false;
    for (uint32_t i = 0; i < count && !damaged; ++i) {
        power_history_record_t record;
        if (!read_record_idx(oldest_slot, i, &record) || !record_is_valid(&record)) {
            /* An unreadable slot is damage too, not something to skip over: a
             * ring whose readable records happen to be in order must still be
             * compacted, otherwise the repair never runs and the ring keeps
             * growing back into the broken region (the "ring stalls at the
             * same record count" fault). */
            damaged = true;
            continue;
        }
        (void)power_history_migrate_record(&record);
        if (have_prev && record.interval_start_utc <= prev) {
            damaged = true; /* out of order or duplicate */
        }
        prev = record.interval_start_utc;
        have_prev = true;
        if (!repair_pacer_step(&pacer)) {
            ESP_LOGW(TAG, "History repair scan exceeded its time budget; "
                          "leaving the ring for a later boot");
            return true; /* best-effort: boot on, retry next time */
        }
    }
    if (!damaged) {
        return true; /* already sorted and duplicate-free */
    }

    if ((size_t)count <= (size_t)repair_keep_limit()) {
        uint32_t want = count;
        power_history_record_t *records = repair_alloc(&want);
        if (records != NULL && want < count) {
            /* The buffer was clamped below the ring; this path must hold all
             * `count` records, so hand it to the "keep most recent" path
             * instead of overflowing. */
            heap_caps_free(records);
            records = NULL;
        }
        if (records != NULL) {
            size_t valid = 0;
            bool scan_ok = true;
            for (uint32_t i = 0; i < count; ++i) {
                power_history_record_t record = {0};
                if (!read_record_idx(oldest_slot, i, &record) ||
                    !record_is_valid(&record)) {
                    continue;
                }
                (void)power_history_migrate_record(&record);
                records[valid++] = record;
                if (!repair_pacer_step(&pacer)) {
                    scan_ok = false;
                    break;
                }
            }
            if (!scan_ok) {
                heap_caps_free(records);
                ESP_LOGW(TAG, "History repair (RAM) exceeded its time budget; "
                              "leaving the ring for a later boot");
                return true;
            }
            /* Stable insertion sort: the store is "almost sorted" (live minutes
             * in order, a few backfill blocks), so this is fast. */
            for (size_t i = 1; i < valid; ++i) {
                power_history_record_t key = records[i];
                size_t j = i;
                while (j > 0 &&
                       record_precedes(&key, &records[j - 1])) {
                    records[j] = records[j - 1];
                    --j;
                }
                records[j] = key;
                if (!repair_pacer_step(&pacer)) {
                    heap_caps_free(records);
                    ESP_LOGW(TAG, "History repair (RAM sort) exceeded its time budget; "
                                  "leaving the ring for a later boot");
                    return true;
                }
            }
            size_t unique = 0;
            for (size_t i = 0; i < valid; ++i) {
                if (unique != 0 &&
                    records[i].interval_start_utc ==
                        records[unique - 1].interval_start_utc) {
                    continue;
                }
                records[unique++] = records[i];
            }
            uint32_t next_sequence = g_history_header.next_sequence;
            uint32_t oldest_sequence = next_sequence - (uint32_t)unique;
            if (oldest_sequence == 0) {
                oldest_sequence = 1;
            }
            bool ok = true;
            for (size_t i = 0; i < unique && ok; ++i) {
                records[i].sequence = oldest_sequence + (uint32_t)i;
                records[i].checksum = record_checksum(&records[i]);
                ok = write_record_idx(oldest_slot, (uint32_t)i, &records[i]);
            }
            heap_caps_free(records);
            if (!ok || !finish_repair(oldest_slot, (uint32_t)unique)) {
                return false;
            }
            ESP_LOGW(TAG,
                     "Repaired history ring in RAM: %u records, %u duplicate(s) dropped",
                     (unsigned)unique, (unsigned)(valid - unique));
            return true;
        }
    }

    /* Full store: keep the most recent records that fit one RAM buffer, sort
     * them in RAM and write the compacted result back. The in-place disk sort
     * (ring_heap_sort) is deliberately gone: a single SPIFFS lseek on a
     * damaged/full store can hang for minutes and take the device down. A
     * device that keeps running with a shorter history beats one that wedges;
     * the sender backfill re-imports the dropped minutes anyway. */
    uint32_t keep = repair_keep_limit();
    if (keep > count) {
        keep = count;
    }
    if (keep < POWER_HISTORY_REPAIR_MIN_RECORDS) {
        ESP_LOGW(TAG, "History repair: only %u records fit the free heap; "
                      "leaving the ring for a later boot",
                 (unsigned)keep);
        return true;
    }
    uint32_t first = count - keep;
    ESP_LOGW(TAG,
             "History ring too large for one RAM buffer (%u records); repairing "
             "the most recent %u", (unsigned)count, (unsigned)keep);
    uint32_t want = keep;
    power_history_record_t *records = repair_alloc(&want);
    if (records == NULL) {
        ESP_LOGE(TAG, "Could not allocate repair buffer; leaving ring as-is");
        return true;
    }
    /* The buffer may have been clamped below `keep`; scan the most recent
     * `want` records (a smaller, still-consistent slice beats no repair). */
    keep = want;
    first = count - keep;
    size_t valid = 0;
    bool scan_ok = true;
    for (uint32_t i = first; i < count; ++i) {
        power_history_record_t record = {0};
        if (!read_record_idx(oldest_slot, i, &record) ||
            !record_is_valid(&record)) {
            continue;
        }
        (void)power_history_migrate_record(&record);
        records[valid++] = record;
        if (!repair_pacer_step(&pacer)) {
            scan_ok = false;
            break;
        }
    }
    if (!scan_ok) {
        heap_caps_free(records);
        ESP_LOGW(TAG, "History repair (keep) exceeded its time budget; "
                      "leaving the ring for a later boot");
        return true;
    }
    for (size_t i = 1; i < valid; ++i) {
        power_history_record_t key = records[i];
        size_t j = i;
        while (j > 0 && record_precedes(&key, &records[j - 1])) {
            records[j] = records[j - 1];
            --j;
        }
        records[j] = key;
        if (!repair_pacer_step(&pacer)) {
            heap_caps_free(records);
            ESP_LOGW(TAG, "History repair (keep sort) exceeded its time budget; "
                          "leaving the ring for a later boot");
            return true;
        }
    }
    size_t unique = 0;
    for (size_t i = 0; i < valid; ++i) {
        if (unique != 0 &&
            records[i].interval_start_utc ==
                records[unique - 1].interval_start_utc) {
            continue;
        }
        records[unique++] = records[i];
    }
    uint32_t next_sequence = g_history_header.next_sequence;
    uint32_t oldest_sequence = next_sequence - (uint32_t)unique;
    if (oldest_sequence == 0) {
        oldest_sequence = 1;
    }
    bool ok = true;
    for (size_t i = 0; i < unique && ok; ++i) {
        records[i].sequence = oldest_sequence + (uint32_t)i;
        records[i].checksum = record_checksum(&records[i]);
        ok = write_record_idx(oldest_slot, (uint32_t)i, &records[i]);
    }
    heap_caps_free(records);
    if (!ok || !finish_repair(oldest_slot, (uint32_t)unique)) {
        return false;
    }
    ESP_LOGW(TAG,
             "Repaired full history ring in RAM (kept most recent %u): "
             "%u records, %u duplicate(s) dropped",
             (unsigned)keep, (unsigned)unique, (unsigned)(valid - unique));
    return true;
}

/* Background repair task. The device must boot and serve (Wi-Fi, HTTP, OTA,
 * telemetry) before the store is touched: a damaged/full SPIFFS store can make
 * a single lseek hang for minutes, and the repair must never take the boot or
 * the web server down with it. Runs once, shortly after boot, at low priority;
 * if the store needs no repair it returns immediately. */
static void power_history_repair_worker(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(POWER_HISTORY_REPAIR_DELAY_MS));
    esp_err_t err = power_history_repair_ring();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "History repair deferred/failed (%s); retrying next boot",
                 esp_err_to_name(err));
    }
    /* One-shot task: it must delete itself. A FreeRTOS task function that
     * returns is undefined behaviour; in this IDF build the task wrapper that
     * would turn it into a clear abort() is compiled out, so returning
     * crashed the receiver on the first boot of this image and the bootloader
     * rolled it back to the previous slot. */
    vTaskDelete(NULL);
}

/* Reformat the storage partition and recreate the history file with fresh
 * headers. The caller must hold the store lock (or run before the writer starts,
 * as in power_history_init). Returns true when a usable, freshly headed file is
 * open. This is the last-resort automatic recovery: the stored minutes are
 * regenerable (the sender backfill re-imports them), so an empty persistent
 * store beats a permanent RAM-only one. */
static bool reformat_store_locked(void)
{
    if (g_history_file != NULL) {
        fclose(g_history_file);
        g_history_file = NULL;
    }
    esp_err_t err = esp_spiffs_format(POWER_HISTORY_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not format persistent SPIFFS partition: %s",
                 esp_err_to_name(err));
        return false;
    }
    g_history_file = fopen(POWER_HISTORY_FILE, "w+b");
    if (g_history_file == NULL) {
        ESP_LOGE(TAG, "Could not recreate persistent history file after format");
        return false;
    }
    power_history_make_fresh_header(&g_history_header);
    power_history_header_t previous = g_history_header;
    previous.generation = 0;
    previous.checksum = header_checksum(&previous);
    if (!write_header_copy(0, &previous) ||
        !write_header_copy(1, &g_history_header)) {
        ESP_LOGE(TAG, "Could not write fresh headers after format");
        fclose(g_history_file);
        g_history_file = NULL;
        return false;
    }
    g_active_header_copy = 1;
    ESP_LOGW(TAG, "Persistent history partition reformatted (fresh store)");
    return true;
}

/* Adopt the on-disk header from an already-open store file (newest valid copy).
 * Returns false when neither copy is valid, so the caller treats the store as
 * fresh. Caller holds the lock and g_history_file is open. */
static bool adopt_existing_header_locked(void)
{
    power_history_header_t headers[2] = {0};
    bool valid[2] = {false, false};
    for (uint8_t i = 0; i < 2; ++i) {
        if (fseek(g_history_file, (long)(i * sizeof(headers[i])), SEEK_SET) == 0 &&
            fread(&headers[i], sizeof(headers[i]), 1, g_history_file) == 1) {
            valid[i] = header_is_valid(&headers[i]);
        }
    }
    if (!valid[0] && !valid[1]) {
        return false;
    }
    if (valid[0] && valid[1]) {
        g_active_header_copy =
            (int32_t)(headers[1].generation - headers[0].generation) > 0 ? 1 : 0;
    } else {
        g_active_header_copy = valid[1] ? 1 : 0;
    }
    g_history_header = headers[g_active_header_copy];
    return true;
}

/* Last-resort automatic recovery, run from the background repair task (never
 * from app_main: a format can block for seconds). It covers both ways the store
 * ends up RAM-only, trying the data-preserving fix first:
 *   1. heal SPIFFS' index in place and retry the open (keeps the minutes);
 *   2. only if that fails: format the partition and recreate the file with fresh
 *      headers (an empty persistent store still beats a permanent RAM-only one,
 *      because the sender backfill re-imports the minutes).
 * The caller must hold g_history_mutex. Returns true once g_history_file is
 * open. */
static bool recover_store_locked(void)
{
    if (g_history_file != NULL) {
        return true;
    }

    /* 1) Gentle: heal the index in place, then retry the open. */
    if (esp_spiffs_mounted(POWER_HISTORY_PARTITION)) {
        (void)esp_spiffs_check(POWER_HISTORY_PARTITION);
        g_history_file = fopen(POWER_HISTORY_FILE, "r+b");
        if (g_history_file != NULL && adopt_existing_header_locked()) {
            g_history_ram_fallback = false;
            g_history_ram_count = 0;
            ESP_LOGW(TAG, "Persistent history recovered in place (index healed, "
                          "stored minutes kept)");
            return true;
        }
        if (g_history_file != NULL) {
            fclose(g_history_file);
            g_history_file = NULL;
        }
    }

    /* 2) Last resort: format the partition and recreate the file. */
    if (!esp_spiffs_mounted(POWER_HISTORY_PARTITION)) {
        esp_err_t err = esp_spiffs_format(POWER_HISTORY_PARTITION);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Explicit SPIFFS format failed: %s",
                     esp_err_to_name(err));
            return false;
        }
        esp_vfs_spiffs_conf_t retry = {
            .base_path = POWER_HISTORY_MOUNT_PATH,
            .partition_label = POWER_HISTORY_PARTITION,
            .max_files = 1,
            .format_if_mount_failed = true,
        };
        err = esp_vfs_spiffs_register(&retry);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS still unmountable after format: %s",
                     esp_err_to_name(err));
            return false;
        }
        ESP_LOGW(TAG, "SPIFFS partition reformatted and remounted");
    }
    if (!reformat_store_locked()) {
        return false;
    }
    g_history_ram_fallback = false;
    g_history_ram_count = 0;
    g_write_errors = 0;
    g_queue_drops = 0;
    ESP_LOGW(TAG, "Persistent history recovered automatically (fresh store; "
                  "run 'pull' to refill from the sender)");
    return true;
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

    /* An older firmware could leave the ring time-unsorted after backfill
     * merges. The repair does NOT run here: a damaged/full SPIFFS store can
     * make a single lseek hang for minutes, which used to wedge app_main
     * before the web server started (TWDT boot-loop). It now runs in its own
     * low-priority background task shortly after boot (see
     * power_history_repair_worker below), so HTTP/OTA/telemetry are up first
     * and the ring is healed -- or dropped to the most recent records --
     * without ever taking the device down. */
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

    /* Record the partition fill once: a store that is ~88% full still reads
     * fine but has little room for SPIFFS garbage collection, so writes start
     * failing with ENOSPC while reads keep working. Having used/total in the
     * status/diag makes that visible without a separate query. */
    size_t spiffs_total = 0;
    size_t spiffs_used = 0;
    if (esp_spiffs_info(POWER_HISTORY_PARTITION, &spiffs_total, &spiffs_used) == ESP_OK) {
        g_storage_total_bytes = (uint32_t)spiffs_total;
        g_storage_used_bytes = (uint32_t)spiffs_used;
    }

    /* Start the background repair AFTER the writer, so the store is fully
     * usable from the first moment and the repair (which takes the same
     * mutex) can never race init. It self-delays again, so HTTP/OTA are up
     * well before any SPIFFS repair loop runs. */
    BaseType_t repair_result = xTaskCreate(
        power_history_repair_worker, "ph_repair", POWER_HISTORY_REPAIR_STACK,
        NULL, 1, NULL);
    if (repair_result != pdPASS) {
        ESP_LOGW(TAG, "Could not start background history repair task");
    }

    ESP_LOGI(TAG, "Persistent history ready: %lu of %u minute records",
             (unsigned long)g_history_header.record_count,
             POWER_HISTORY_CAPACITY);
    return ESP_OK;
}

esp_err_t power_history_format(void)
{
    if (g_history_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Could not lock history store for formatting");
        return ESP_ERR_TIMEOUT;
    }
    if (g_history_file != NULL) {
        fclose(g_history_file);
        g_history_file = NULL;
    }
    /* Format the whole partition, then recreate the history file with fresh
     * headers. The sender backfill re-imports the minute history afterwards, so
     * a damaged ring is replaced cleanly instead of being read around forever. */
    esp_err_t err = esp_spiffs_format(POWER_HISTORY_PARTITION);
    if (err != ESP_OK) {
        xSemaphoreGive(g_history_mutex);
        ESP_LOGE(TAG, "Could not format persistent SPIFFS partition: %s",
                 esp_err_to_name(err));
        return err;
    }
    g_history_file = fopen(POWER_HISTORY_FILE, "w+b");
    if (g_history_file == NULL) {
        xSemaphoreGive(g_history_mutex);
        ESP_LOGE(TAG, "Could not recreate persistent history file after format");
        return ESP_FAIL;
    }
    power_history_make_fresh_header(&g_history_header);
    power_history_header_t previous = g_history_header;
    previous.generation = 0;
    previous.checksum = header_checksum(&previous);
    bool headers_ok = write_header_copy(0, &previous) &&
                      write_header_copy(1, &g_history_header);
    if (!headers_ok) {
        ESP_LOGE(TAG, "Could not write fresh headers after format");
    }
    g_active_header_copy = 1;
    g_history_ram_fallback = false;
    g_history_ram_count = 0;
    g_splice_batches = 0;
    g_splice_records = 0;
    g_splice_skipped = 0;
    g_write_errors = 0;
    g_queue_drops = 0;
    g_storage_used_bytes = 0;
    g_storage_total_bytes = 0;
    xSemaphoreGive(g_history_mutex);
    ESP_LOGW(TAG, "Persistent history partition reformatted; waiting for sender backfill");
    return headers_ok ? ESP_OK : ESP_FAIL;
}

static bool history_enqueue_impl(const power_history_record_t *record, TickType_t wait)
{
    if (!g_history_ready || record == NULL) {
        portENTER_CRITICAL(&g_stats_lock);
        ++g_queue_drops;
        portEXIT_CRITICAL(&g_stats_lock);
        ESP_LOGE(TAG, "History enqueue rejected: %s",
                 record == NULL ? "null record" : "store not ready");
        return false;
    }
    if (xQueueSend(g_history_queue, record, wait) != pdTRUE) {
        /* A full queue means the writer is slower than the producers (a stalled
         * or GC-thrashing SPIFFS). That is a different fault from an invalid
         * call, so name it instead of lumping both into one message. */
        portENTER_CRITICAL(&g_stats_lock);
        ++g_queue_drops;
        portEXIT_CRITICAL(&g_stats_lock);
        ESP_LOGE(TAG, "History queue full (%u slots); dropping minute",
                 (unsigned)POWER_HISTORY_QUEUE_LENGTH);
        return false;
    }
    return true;
}

bool power_history_enqueue(const power_history_record_t *record)
{
    /* Non-blocking: this runs from the 1 Hz sample timer, which must not stall
     * waiting for SPIFFS. A single lost minute is recovered by the backfill. */
    return history_enqueue_impl(record, 0);
}

bool power_history_enqueue_wait(const power_history_record_t *record, uint32_t timeout_ms)
{
    /* Bounded wait for burst producers that run in task context (the boot-marker
     * fill queues up to BOOT_MARKER_MAX_MINUTES records in one go). The writer
     * drains in small batches, so a non-blocking send dropped the tail of the
     * burst right after a boot ("History queue full (16 slots)"). Never call
     * this from an ISR or a timer callback. */
    return history_enqueue_impl(record, pdMS_TO_TICKS(timeout_ms));
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

esp_err_t power_history_import_sorted(const power_history_record_t *records,
                                      size_t count, size_t *imported_out,
                                      size_t *skipped_out)
{
    if (imported_out != NULL) {
        *imported_out = 0;
    }
    if (skipped_out != NULL) {
        *skipped_out = 0;
    }
    if (records == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_history_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_history_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t imported = 0;
    size_t skipped = 0;

    if (g_history_file == NULL) {
        /* RAM-only fallback: the ring holds one page and is append-ordered, so a
         * restore is best-effort here; anything past the page is skipped. */
        for (size_t i = 0; i < count; ++i) {
            if (g_history_ram_count >= POWER_HISTORY_PAGE_SIZE) {
                ++skipped;
                continue;
            }
            if (records[i].interval_start_utc < POWER_HISTORY_MIN_VALID_UTC) {
                ++skipped;
                continue;
            }
            power_history_record_t record = records[i];
            record.sequence = g_history_header.next_sequence;
            record.format_version = POWER_HISTORY_VERSION;
            record.reserved = 0;
            record.checksum = record_checksum(&record);
            if (++g_history_header.next_sequence == 0) {
                g_history_header.next_sequence = 1;
            }
            g_history_ram_records[g_history_ram_count++] = record;
            notify_store_observer(&record);
            ++imported;
        }
        xSemaphoreGive(g_history_mutex);
        if (imported_out != NULL) {
            *imported_out = imported;
        }
        if (skipped_out != NULL) {
            *skipped_out = skipped;
        }
        return ESP_OK;
    }

    power_history_header_t updated = g_history_header;
    for (size_t i = 0; i < count; ++i) {
        power_history_record_t record = records[i];
        /* A restore must never push a live minute out of a full ring: drop the
         * imported record instead of the oldest stored one. */
        if (updated.record_count >= POWER_HISTORY_CAPACITY) {
            ++skipped;
            continue;
        }
        /* Same valid-clock guard as the live/merge path: a 0/1970 minute would
         * sort before every stored minute and break the time-ordered ring. */
        if (record.interval_start_utc < POWER_HISTORY_MIN_VALID_UTC) {
            ++skipped;
            continue;
        }
        if (minute_is_stored_geom(updated.next_slot, updated.record_count,
                                  record.interval_start_utc)) {
            ++skipped;
            continue;
        }
        /* Insert at the time-ordered position like the live/merge path, so a
         * restore also keeps the ring sorted (and therefore dedupe and the CSV
         * paging correct) even when the uploaded minutes are older than the
         * local tail or the local ring is already out of order. */
        uint32_t oldest_slot = oldest_slot_of(updated.next_slot, updated.record_count);
        uint32_t oldest_sequence = updated.next_sequence - updated.record_count;
        uint32_t at = find_first_at_or_after_time(oldest_slot, updated.record_count,
                                                  record.interval_start_utc);
        record.sequence = oldest_sequence + at;
        if (record.sequence == 0) {
            record.sequence = 1;
        }
        record.format_version = POWER_HISTORY_VERSION;
        record.reserved = 0;
        record.checksum = record_checksum(&record);
        if (at < updated.record_count) {
            /* Same splice bound as the live/merge path: a restore that asks to
             * insert a minute deep below the tail is dropped instead of
             * rewriting the whole newer ring under the mutex. */
            if (updated.record_count - at > POWER_HISTORY_MAX_SPLICE_RECORDS) {
                ++g_splice_skipped;
                ++skipped;
                continue;
            }
            if (!shift_up_renumber(oldest_slot, updated.record_count, at)) {
                xSemaphoreGive(g_history_mutex);
                if (imported_out != NULL) {
                    *imported_out = imported;
                }
                if (skipped_out != NULL) {
                    *skipped_out = skipped;
                }
                return ESP_FAIL;
            }
            if (!write_record_idx(oldest_slot, at, &record)) {
                xSemaphoreGive(g_history_mutex);
                if (imported_out != NULL) {
                    *imported_out = imported;
                }
                if (skipped_out != NULL) {
                    *skipped_out = skipped;
                }
                return ESP_FAIL;
            }
        } else {
            long offset = (long)(2u * sizeof(power_history_header_t) +
                                 updated.next_slot * sizeof(record));
            if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
                fwrite(&record, sizeof(record), 1, g_history_file) != 1) {
                xSemaphoreGive(g_history_mutex);
                if (imported_out != NULL) {
                    *imported_out = imported;
                }
                if (skipped_out != NULL) {
                    *skipped_out = skipped;
                }
                return ESP_FAIL;
            }
        }
        notify_store_observer(&record);
        ++updated.next_sequence;
        if (updated.next_sequence == 0) {
            updated.next_sequence = 1;
        }
        ++updated.record_count;
        updated.next_slot = (updated.next_slot + 1u) % POWER_HISTORY_CAPACITY;
        ++imported;
    }

    if (imported != 0) {
        if (fflush(g_history_file) != 0 || fsync(fileno(g_history_file)) != 0) {
            xSemaphoreGive(g_history_mutex);
            return ESP_FAIL;
        }
        ++updated.generation;
        updated.checksum = header_checksum(&updated);
        uint8_t next_copy = (uint8_t)(1u - g_active_header_copy);
        if (!write_header_copy(next_copy, &updated)) {
            xSemaphoreGive(g_history_mutex);
            return ESP_FAIL;
        }
        g_history_header = updated;
        g_active_header_copy = next_copy;
    }

    xSemaphoreGive(g_history_mutex);
    if (imported_out != NULL) {
        *imported_out = imported;
    }
    if (skipped_out != NULL) {
        *skipped_out = skipped;
    }
    return ESP_OK;
}

/* Fill the status fields that need no store lock: readiness, the stats counters
 * and the SPIFFS fill. The header-derived fields (record_count/latest/oldest)
 * stay zero here; the caller fills them while it holds g_history_mutex. A read
 * path uses this so it can take the lock exactly once, instead of get_status()
 * taking it and the read taking it again - under backfill load that second wait
 * alone pushed every read past the timeout and the dashboard showed an empty
 * graph even though the ring was full. */
static void status_fill_unlocked(power_history_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->ready = g_history_ready;
    /* `ready` is also true in the RAM-only fallback, so report the real
     * persistence separately: only an open SPIFFS file survives a reboot. */
    status->persistent = g_history_ready && g_history_file != NULL;
    portENTER_CRITICAL(&g_stats_lock);
    status->queue_drops = g_queue_drops;
    status->write_errors = g_write_errors;
    portEXIT_CRITICAL(&g_stats_lock);
    /* Splice counters and the SPIFFS fill are written by the writer under
     * g_history_mutex; read them without it (single-word reads, the exact value
     * is not critical for a diagnostic). */
    status->splice_batches = g_splice_batches;
    status->splice_records = g_splice_records;
    status->splice_skipped = g_splice_skipped;
    status->storage_used_bytes = g_storage_used_bytes;
    status->storage_total_bytes = g_storage_total_bytes;
}

/* Fill the header-derived status fields. The caller must hold g_history_mutex. */
static void status_fill_header_locked(power_history_status_t *status)
{
    status->record_count = g_history_header.record_count;
    status->latest_sequence = g_history_header.next_sequence - 1u;
    if (status->record_count != 0) {
        status->oldest_sequence =
            g_history_header.next_sequence - status->record_count;
        if (status->oldest_sequence == 0) {
            status->oldest_sequence = 1;
        }
    }
}

void power_history_get_status(power_history_status_t *status)
{
    if (status == NULL) {
        return;
    }
    status_fill_unlocked(status);
    if (g_history_ready && xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(POWER_HISTORY_READ_LOCK_TIMEOUT_MS)) == pdTRUE) {
        status_fill_header_locked(status);
        xSemaphoreGive(g_history_mutex);
    }
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
    status_fill_unlocked(status);
    if (!status->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(POWER_HISTORY_READ_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    status_fill_header_locked(status);

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
            /* A physically unreadable slot (EIO from a damaged SPIFFS file,
             * e.g. leftovers of an interrupted splice) must not take the whole
             * history down with a 500. Skip it; a later format + sender
             * backfill will restore the minute. Reading on keeps the dashboard
             * graph usable even while the store is partially damaged. */
            ESP_LOGW(TAG, "Skipping unreadable history record at slot %lu",
                     (unsigned long)slot);
            continue;
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
    status_fill_unlocked(status);
    if (!status->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(POWER_HISTORY_READ_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    status_fill_header_locked(status);

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
    power_history_record_t block[16];
    uint32_t remaining = g_history_header.record_count - index;
    while (remaining != 0) {
        uint32_t slot = (oldest_slot + index) % POWER_HISTORY_CAPACITY;
        uint32_t chunk = remaining < (uint32_t)(sizeof(block)/sizeof(block[0])) ? remaining : (uint32_t)(sizeof(block)/sizeof(block[0]));
        if (slot + chunk > POWER_HISTORY_CAPACITY) {
            chunk = POWER_HISTORY_CAPACITY - slot;
        }
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(block, sizeof(block[0]), chunk, g_history_file) != chunk) {
            /* Block read hit a physically unreadable slot; recover by reading
             * the block record-by-record and keeping the readable ones, so one
             * damaged slot cannot blank the whole window. */
            size_t got = 0;
            ESP_LOGW(TAG, "History window block read failed at slot %lu; "
                          "recovering per-record", (unsigned long)slot);
            while (got < chunk) {
                uint32_t rslot = (slot + got) % POWER_HISTORY_CAPACITY;
                long roffset = (long)(2u * sizeof(power_history_header_t) +
                                      rslot * sizeof(power_history_record_t));
                if (fseek(g_history_file, roffset, SEEK_SET) != 0 ||
                    fread(&block[got], sizeof(block[0]), 1, g_history_file) != 1) {
                    memset(&block[got], 0, sizeof(block[0]));
                }
                ++got;
            }
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
    status_fill_unlocked(status);
    if (!status->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(POWER_HISTORY_READ_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    status_fill_header_locked(status);

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
    power_history_record_t block[16];
    uint32_t remaining = g_history_header.record_count - index;
    while (remaining != 0) {
        uint32_t slot = (oldest_slot + index) % POWER_HISTORY_CAPACITY;
        uint32_t chunk = remaining < (uint32_t)(sizeof(block)/sizeof(block[0])) ? remaining : (uint32_t)(sizeof(block)/sizeof(block[0]));
        if (slot + chunk > POWER_HISTORY_CAPACITY) {
            chunk = POWER_HISTORY_CAPACITY - slot;
        }
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(block, sizeof(block[0]), chunk, g_history_file) != chunk) {
            /* Block read hit a physically unreadable slot; recover per-record so
             * one damaged slot cannot blank the whole page. */
            size_t got = 0;
            ESP_LOGW(TAG, "History page block read failed at slot %lu; "
                          "recovering per-record", (unsigned long)slot);
            while (got < chunk) {
                uint32_t rslot = (slot + got) % POWER_HISTORY_CAPACITY;
                long roffset = (long)(2u * sizeof(power_history_header_t) +
                                      rslot * sizeof(power_history_record_t));
                if (fseek(g_history_file, roffset, SEEK_SET) != 0 ||
                    fread(&block[got], sizeof(block[0]), 1, g_history_file) != 1) {
                    memset(&block[got], 0, sizeof(block[0]));
                }
                ++got;
            }
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

bool power_history_time_range(int64_t *oldest_utc, int64_t *latest_utc)
{
    if (oldest_utc != NULL) {
        *oldest_utc = 0;
    }
    if (latest_utc != NULL) {
        *latest_utc = 0;
    }
    if (!g_history_ready ||
        xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(POWER_HISTORY_READ_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    int64_t oldest = 0;
    int64_t latest = 0;
    if (g_history_header.record_count != 0) {
        uint32_t oldest_slot =
            (g_history_header.next_slot + POWER_HISTORY_CAPACITY -
             g_history_header.record_count) % POWER_HISTORY_CAPACITY;
        uint32_t newest_slot =
            (g_history_header.next_slot + POWER_HISTORY_CAPACITY - 1u) % POWER_HISTORY_CAPACITY;
        if (g_history_file == NULL) {
            oldest = g_history_ram_records[0].interval_start_utc;
            latest = g_history_ram_records[g_history_ram_count == 0 ? 0 : g_history_ram_count - 1]
                         .interval_start_utc;
        } else {
            power_history_record_t record;
            long offset = (long)(2u * sizeof(power_history_header_t) +
                                 oldest_slot * sizeof(record));
            if (fseek(g_history_file, offset, SEEK_SET) == 0 &&
                fread(&record, sizeof(record), 1, g_history_file) == 1) {
                oldest = record.interval_start_utc;
            }
            offset = (long)(2u * sizeof(power_history_header_t) +
                            newest_slot * sizeof(record));
            if (fseek(g_history_file, offset, SEEK_SET) == 0 &&
                fread(&record, sizeof(record), 1, g_history_file) == 1) {
                latest = record.interval_start_utc;
            }
        }
    }
    xSemaphoreGive(g_history_mutex);
    if (oldest_utc != NULL) {
        *oldest_utc = oldest;
    }
    if (latest_utc != NULL) {
        *latest_utc = latest;
    }
    return true;
}

bool power_history_has_minute(int64_t interval_start_utc)
{
    if (!g_history_ready ||
        xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(POWER_HISTORY_READ_LOCK_TIMEOUT_MS)) != pdTRUE) {
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
    status_fill_unlocked(status);
    if (!status->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_history_mutex, pdMS_TO_TICKS(POWER_HISTORY_READ_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    status_fill_header_locked(status);

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
    /* The ring is time-sorted, so only the slice that overlaps the requested
     * window has to be read: a full 12k-record walk under the mutex took
     * seconds of SPIFFS fseek+fread per dashboard poll and made the HTTP task
     * time out (>8 s) whenever the store grew. Binary-search the first index
     * at/after the window start and the first index past the window end, then
     * walk only that slice. */
    uint32_t start_index = 0;
    uint32_t end_index = g_history_header.record_count;
    if (g_history_header.record_count != 0) {
        /* Same walk order as the loop below: index 0 = oldest_slot. */
        start_index = find_first_at_or_after_time(
            oldest_slot, g_history_header.record_count, window_start_utc);
        if (window_start_utc > INT64_MIN &&
            bucket_seconds <= (INT64_MAX - window_start_utc) / (int64_t)bucket_count) {
            int64_t window_end_utc =
                window_start_utc + bucket_seconds * (int64_t)bucket_count;
            end_index = find_first_at_or_after_time(
                oldest_slot, g_history_header.record_count, window_end_utc);
        }
        if (start_index > end_index) {
            start_index = 0;
            end_index = 0;
        }
    }
    /* Walk the ring once but read in blocks: a large log made the per-record
     * fseek+fread dominate (tens of thousands of syscalls) and blocked the HTTP
     * task. A small block buffer keeps RAM flat and stays correct across the
     * ring wrap. */
    power_history_record_t block[16];
    uint32_t remaining = end_index - start_index;
    uint32_t index = start_index;
    while (remaining != 0) {
        uint32_t slot = (oldest_slot + index) % POWER_HISTORY_CAPACITY;
        uint32_t chunk = remaining < (uint32_t)(sizeof(block)/sizeof(block[0])) ? remaining : (uint32_t)(sizeof(block)/sizeof(block[0]));
        if (slot + chunk > POWER_HISTORY_CAPACITY) {
            chunk = POWER_HISTORY_CAPACITY - slot;
        }
        long offset = (long)(2u * sizeof(power_history_header_t) +
                             slot * sizeof(power_history_record_t));
        if (fseek(g_history_file, offset, SEEK_SET) != 0 ||
            fread(block, sizeof(block[0]), chunk, g_history_file) != chunk) {
            /* Block read hit a physically unreadable slot; recover per-record so
             * one damaged slot cannot blank the whole aggregation. */
            size_t got = 0;
            ESP_LOGW(TAG, "History aggregate block read failed at slot %lu; "
                          "recovering per-record", (unsigned long)slot);
            while (got < chunk) {
                uint32_t rslot = (slot + got) % POWER_HISTORY_CAPACITY;
                long roffset = (long)(2u * sizeof(power_history_header_t) +
                                      rslot * sizeof(power_history_record_t));
                if (fseek(g_history_file, roffset, SEEK_SET) != 0 ||
                    fread(&block[got], sizeof(block[0]), 1, g_history_file) != 1) {
                    memset(&block[got], 0, sizeof(block[0]));
                }
                ++got;
            }
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

