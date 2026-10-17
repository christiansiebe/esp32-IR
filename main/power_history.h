#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* How many minute records the ring buffer holds. This is bounded by what
 * actually fits in the SPIFFS "storage" partition, not by a round number: the
 * on-disk file is 2 headers + CAPACITY * sizeof(record) bytes, and SPIFFS must
 * still keep free blocks for wear-levelling garbage collection. 30000 records
 * (840 kB) did not fit, so once the log filled up every write failed with
 * SPIFFS_ERR_FULL and the dashboard showed recurring write errors. 29000
 * (~20 days) leaves comfortable headroom. If this grows, re-check it against
 * the partition size with components/spiffs/spiffsgen.py. */
#define POWER_HISTORY_CAPACITY 29000u

/* Largest store the startup repair sorts in one RAM buffer (count * 28 B). A
 * bigger store is sorted in place on disk with O(1) RAM, so a full ring is
 * repaired too. Kept low enough that the buffer fits the free heap next to the
 * Wi-Fi stack on a no-PSRAM ESP32. */
#define POWER_HISTORY_REPAIR_RAM_LIMIT 4096u

#define POWER_HISTORY_PAGE_SIZE 128u

/* SPIFFS mount point and history file. Overridable so the reboot-persistence
 * host test can point the store at a scratch directory instead of the
 * device-only /powerlog mount. */
#ifndef POWER_HISTORY_MOUNT_PATH
#define POWER_HISTORY_MOUNT_PATH "/powerlog"
#endif
#ifndef POWER_HISTORY_FILE
#define POWER_HISTORY_FILE POWER_HISTORY_MOUNT_PATH "/quarters.bin"
#endif

/* The SPIFFS partition label holding the measurement store (partitions.csv).
 * Public so the OTA path in main.c can assert an update image never overlaps
 * it; that guard is what keeps an OTA from ever erasing the history. */
#ifndef POWER_HISTORY_PARTITION
#define POWER_HISTORY_PARTITION "storage"
#endif

/* On-disk record/header format version. v1 stored the slot generation in
 * `reserved`; v2 left `reserved` at 0; v3 uses `reason` to mark a minute the
 * sender could not measure (it was offline: restart/OTA/battery). Bumping the
 * version must not drop the stored history: power_history_migrate_record()
 * upgrades older records in place, and only versions outside [OLDEST, VERSION]
 * are rejected. */
#define POWER_HISTORY_VERSION 3u
#define POWER_HISTORY_OLDEST_SUPPORTED_VERSION 1u

/* Why a minute carries no live measurement. 0 is a normal measured minute (or
 * one merged live from the sender); anything else is a "boot marker": the
 * sender filled an offline window with a marker record instead of a value, so
 * the receiver can tell an expected outage apart from a real data loss. */
enum {
    POWER_HISTORY_REASON_NONE = 0,    /* measured (live) */
    POWER_HISTORY_REASON_UNKNOWN = 1, /* offline, cause not determinable */
    POWER_HISTORY_REASON_OTA = 2,     /* planned OTA reboot */
    POWER_HISTORY_REASON_CRASH = 3,   /* panic / watchdog reset */
    POWER_HISTORY_REASON_BROWNOUT = 4,/* battery / power loss */
    POWER_HISTORY_REASON_POWERON = 5, /* cold power-on */
    POWER_HISTORY_REASON_RESTART = 6, /* other deliberate restart */
};
#define POWER_HISTORY_REASON_COUNT 7u

typedef struct __attribute__((packed)) {
    uint32_t sequence;
    int64_t interval_start_utc;
    int32_t average_milliwatts;
    uint32_t sample_count;
    uint16_t format_version;
    uint8_t reason;   /* POWER_HISTORY_REASON_*; 0 = measured */
    uint8_t reserved;
    uint32_t checksum;
} power_history_record_t;

typedef struct {
    bool ready;
    uint32_t oldest_sequence;
    uint32_t latest_sequence;
    uint32_t record_count;
    uint32_t queue_drops;
    uint32_t write_errors;
} power_history_status_t;

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

/* Fill a header with the values a freshly created history file must carry.
 * Exposed (with the validator) so the boot-time header invariants can be
 * covered by host tests without touching SPIFFS. */
void power_history_make_fresh_header(power_history_header_t *header);
bool power_history_header_is_valid(const power_history_header_t *header);

esp_err_t power_history_init(void);
bool power_history_enqueue(const power_history_record_t *record);

/* Merge minute records pulled from the sender (backfill). Each record is
 * validated/migrated first; corrupt or unsupported entries are skipped. A valid
 * record is enqueued only when it is new for the receiver: either its sequence
 * is ahead of the local tail, or (when the sender's sequence counter is ahead
 * because the receiver restarted) its minute is later than the newest local
 * minute. That second rule prevents duplicating already-recorded minutes after
 * a receiver reboot. Sets *merged_out to the number of records taken. */
esp_err_t power_history_merge(const power_history_record_t *records, size_t count,
                              size_t *merged_out);

/* Bulk import for the CSV restore ("lost data back from a backup"). Unlike
 * power_history_merge(), which enqueues for the worker, this writes in one pass
 * under the store mutex and inserts every record at its time-ordered position,
 * so the ring stays sorted by minute (and by sequence) no matter where the
 * uploaded minutes fall relative to the local tail. That keeps the
 * binary-search dedupe and the CSV export correct even when the local ring was
 * left out of order by an older firmware (see repair_ring_if_needed, which runs
 * once at init).
 *
 * `records` need not be sorted and carry no sequence (0); the store assigns
 * sequence/format/checksum. A record whose minute is already stored is skipped
 * as a duplicate, and a record that would evict a newer minute from a full ring
 * is skipped so live data is never overwritten. *imported_out / *skipped_out
 * report the counts. */
esp_err_t power_history_import_sorted(const power_history_record_t *records,
                                      size_t count, size_t *imported_out,
                                      size_t *skipped_out);

#ifdef POWER_HISTORY_TEST_HOOKS
/* Test-only: set the store metadata without SPIFFS so the merge decision can be
 * exercised on the host (the firmware never calls this). */
void power_history_test_set_status(uint32_t oldest_sequence, uint32_t latest_sequence,
                                   uint32_t record_count);
#endif

/* Upgrade an on-disk record to the current format in place, so a format-version
 * bump does not drop the stored history. Returns true for records that are
 * usable afterwards (native OR migrated), false for unsupported versions and
 * for records whose CRC does not match (kept strict across versions — only the
 * storage format changed, the checksum algorithm did not). Recomputes the
 * checksum after a migration because a migration changes the fields the CRC
 * covers. Idempotent for the current version. Exposed for host tests. */
bool power_history_migrate_record(power_history_record_t *record);

/* Machine token for a record reason ("none", "unknown", "ota", "crash",
 * "brownout", "poweron", "restart"). Used by the CSV export and the JSON. */
const char *power_history_reason_token(uint8_t reason);

/* Map an esp_reset_reason_t value (ESP-IDF numbering) plus the "planned OTA
 * reboot" flag to a POWER_HISTORY_REASON_*. Pure, so the boot-marker decision is
 * host-testable without ESP-IDF. */
uint8_t power_history_reason_from_reset(int reset_reason, bool deliberate_ota);

/* Summary of a minute window for the "is anything really lost?" answer. */
typedef struct {
    uint32_t total;      /* minutes in [from_utc, to_utc) */
    uint32_t present;    /* a record exists (measured or boot marker) */
    uint32_t missing;    /* no record at all -> real loss */
    uint32_t expected;   /* present AND reason != 0 (offline/OTA/crash/battery) */
    uint32_t measured;   /* present AND reason == 0 */
    uint32_t reasons[POWER_HISTORY_REASON_COUNT];
} power_history_gap_summary_t;

/* Fill *summary for the minutes in [from_utc, to_utc) (both rounded down to a
 * whole minute). Missing minutes are the only real loss; "expected" minutes are
 * boot markers the sender wrote for an offline window. */
esp_err_t power_history_summarize_window(int64_t from_utc, int64_t to_utc,
                                         power_history_gap_summary_t *summary);
esp_err_t power_history_read(uint32_t after_sequence, uint32_t recent_count,
                             power_history_record_t *records, size_t capacity,
                             size_t *record_count, power_history_status_t *status,
                             bool *overflow);
/* Page read for the ESP-NOW history backfill: return up to max_records records
 * with `sequence > after_sequence` whose `interval_start_utc` lies in
 * [from_utc, to_utc). Records are ordered by time, so the caller pages forward
 * by passing the last returned sequence. `*more` is set while further matching
 * records remain. Unlike power_history_read() it applies no recent_count/oldest
 * clamp, so an explicit server-side window is honoured exactly. */
esp_err_t power_history_read_window(int64_t from_utc, int64_t to_utc,
                                    uint32_t after_sequence, uint32_t max_records,
                                    power_history_record_t *records, size_t capacity,
                                    size_t *record_count, power_history_status_t *status,
                                    bool *more);
/* Time-ordered page read for a full-store sync: up to max_records records with
 * interval_start_utc > after_utc (oldest first). Paging forward by the last
 * returned minute walks the whole ring, including the oldest record that
 * power_history_read_window() cannot reach with a zero time cursor. `*more` is
 * set while further records follow. Records are validated/migrated on the way
 * out; corrupt entries are skipped. */
esp_err_t power_history_read_after(int64_t after_utc, uint32_t max_records,
                                   power_history_record_t *records, size_t capacity,
                                   size_t *record_count, power_history_status_t *status,
                                   bool *more);
/* True when a valid minute with that interval_start_utc is already stored.
 * Backs the full-store sync's per-minute dedupe so the receiver never merges a
 * minute it already holds. Binary search, so it stays cheap on a full store. */
bool power_history_has_minute(int64_t interval_start_utc);

/* Real stored time range (oldest/latest record minute). Used to report the
 * sender's true data horizon over ESP-NOW, so the receiver can tell whether it
 * has caught up instead of comparing against a synthetic "now" bound. Returns
 * false (and leaves both at 0) while the store is not ready or empty. */
bool power_history_time_range(int64_t *oldest_utc, int64_t *latest_utc);
/* True when the record's minute interval falls inside [from_utc, to_utc). The
 * record is validated (and migrated in a local copy) first, so corrupt or
 * unsupported entries are rejected. Exposed so the backfill window filter is
 * host-testable without SPIFFS. */
bool power_history_record_in_window(const power_history_record_t *record,
                                    int64_t from_utc, int64_t to_utc);
/* Single-Pass-Aggregation: iteriert alle gespeicherten Saetze genau einmal und
 * mittelt sie in feste Zeit-Buckets. Vermeidet O(n^2)-Paging bei vollem
 * Speicher (29000 Saetze). count != 0 markiert belegte Buckets. */
esp_err_t power_history_aggregate(int64_t window_start_utc, int64_t bucket_seconds,
                                  double *sum_w, uint32_t *count, int64_t *bucket_start_ms,
                                  size_t bucket_count, power_history_status_t *status);

/* Reine, hardwareunabhaengige Aggregations-Helfer (ohne NVS/SPIFFS/FreeRTOS),
 * damit dieselbe Logik auf dem Host getestet werden kann. */
void power_history_aggregate_init(int64_t window_start_utc, int64_t bucket_seconds,
                                  double *sum_w, uint32_t *count,
                                  int64_t *bucket_start_ms, size_t bucket_count);
esp_err_t power_history_aggregate_core(int64_t window_start_utc,
                                       int64_t bucket_seconds,
                                       const power_history_record_t *records,
                                       size_t record_count, double *sum_w,
                                       uint32_t *count, size_t bucket_count);
void power_history_get_status(power_history_status_t *status);
