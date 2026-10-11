#include "crash_journal.h"

#include <stddef.h>
#include <string.h>

static uint32_t crc32_bytes(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

void crash_journal_make_fresh(crash_journal_t *journal, uint32_t boot_count,
                              int32_t reset_reason)
{
    memset(journal, 0, sizeof(*journal));
    journal->magic = CRASH_JOURNAL_MAGIC;
    journal->version = CRASH_JOURNAL_VERSION;
    journal->boot_count = boot_count;
    journal->reset_reason = reset_reason;
    journal->clean_shutdown = 0;
    crash_journal_finalize(journal);
}

void crash_journal_set_events(crash_journal_t *journal,
                              const crash_journal_event_t *events, size_t count)
{
    if (count > CRASH_JOURNAL_EVENT_COUNT) {
        /* Keep the newest events (the tail of the chronological ring). */
        events += count - CRASH_JOURNAL_EVENT_COUNT;
        count = CRASH_JOURNAL_EVENT_COUNT;
    }
    journal->event_count = (uint8_t)count;
    if (count != 0 && events != NULL) {
        memcpy(journal->events, events, count * sizeof(journal->events[0]));
    }
    crash_journal_finalize(journal);
}

uint32_t crash_journal_checksum(const crash_journal_t *journal)
{
    return crc32_bytes((const uint8_t *)journal,
                       offsetof(crash_journal_t, checksum));
}

void crash_journal_finalize(crash_journal_t *journal)
{
    journal->checksum = crash_journal_checksum(journal);
}

bool crash_journal_is_valid(const crash_journal_t *journal)
{
    return journal->magic == CRASH_JOURNAL_MAGIC &&
           journal->version == CRASH_JOURNAL_VERSION &&
           journal->event_count <= CRASH_JOURNAL_EVENT_COUNT &&
           journal->checksum == crash_journal_checksum(journal);
}

const char *crash_journal_reset_reason_name(int reason)
{
    /* Values match esp_reset_reason_t in ESP-IDF. */
    switch (reason) {
    case 0: return "unknown";
    case 1: return "power-on";
    case 2: return "external-pin";
    case 3: return "software-restart";
    case 4: return "panic";
    case 5: return "interrupt-watchdog";
    case 6: return "task-watchdog";
    case 7: return "watchdog";
    case 8: return "deep-sleep";
    case 9: return "brownout";
    case 10: return "sdio";
    default: return "other";
    }
}
