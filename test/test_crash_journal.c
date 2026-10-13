/* Host tests for the crash-journal serialization helpers.
 *
 * These are pure (no NVS, no ESP-IDF): they pin down the checksum, the
 * validity rules and the "keep the newest events" behaviour that the sender
 * relies on after a crash. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crash_journal.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

static crash_journal_event_t make_event(uint64_t ts, uint8_t level, const char *text)
{
    crash_journal_event_t e;
    memset(&e, 0, sizeof(e));
    e.timestamp_ms = ts;
    e.level = level;
    snprintf(e.text, sizeof(e.text), "%s", text);
    return e;
}

int main(void)
{
    crash_journal_t journal;

    crash_journal_make_fresh(&journal, 42, 4 /* panic */);
    CHECK(crash_journal_is_valid(&journal), "fresh journal is valid");
    CHECK(journal.boot_count == 42, "boot count stored");
    CHECK(journal.reset_reason == 4, "reset reason stored");
    CHECK(journal.event_count == 0, "fresh journal has no events");
    CHECK(strcmp(crash_journal_reset_reason_name(4), "panic") == 0,
          "reset reason name for panic");
    CHECK(strcmp(crash_journal_reset_reason_name(9), "brownout") == 0,
          "reset reason name for brownout");
    CHECK(strcmp(crash_journal_reset_reason_name(1234), "other") == 0,
          "unknown reset reason falls back to other");
    CHECK(crash_journal_reset_is_deliberate(3), "software restart is deliberate");
    CHECK(crash_journal_reset_is_deliberate(8), "deep sleep is deliberate");
    CHECK(!crash_journal_reset_is_deliberate(4), "panic is not deliberate");
    CHECK(!crash_journal_reset_is_deliberate(6), "task watchdog is not deliberate");
    CHECK(!crash_journal_reset_is_deliberate(1), "power-on is not deliberate");

    crash_journal_event_t events[3] = {
        make_event(1000, 2, "uart fifo overflow"),
        make_event(2000, 1, "sml send timeout seq=7"),
        make_event(3000, 2, "parser fault"),
    };
    crash_journal_set_events(&journal, events, 3);
    CHECK(crash_journal_is_valid(&journal), "journal with events is valid");
    CHECK(journal.event_count == 3, "all events stored");
    CHECK(journal.events[0].timestamp_ms == 1000, "first event in order");
    CHECK(journal.events[2].timestamp_ms == 3000, "last event in order");
    CHECK(strcmp(journal.events[1].text, "sml send timeout seq=7") == 0,
          "event text preserved");

    /* More events than fit: the newest ones must win. */
    crash_journal_event_t many[CRASH_JOURNAL_EVENT_COUNT + 3];
    for (size_t i = 0; i < sizeof(many) / sizeof(many[0]); ++i) {
        many[i] = make_event(1000 + i, 0, "e");
    }
    crash_journal_set_events(&journal, many, sizeof(many) / sizeof(many[0]));
    CHECK(journal.event_count == CRASH_JOURNAL_EVENT_COUNT,
          "event count clamped to capacity");
    CHECK(journal.events[0].timestamp_ms == 1000 + 3,
          "oldest events dropped, newest kept");
    CHECK(journal.events[CRASH_JOURNAL_EVENT_COUNT - 1].timestamp_ms ==
              1000 + (CRASH_JOURNAL_EVENT_COUNT + 2),
          "newest event kept");

    /* A single corrupted byte must invalidate the record. */
    crash_journal_make_fresh(&journal, 1, 1);
    crash_journal_t corrupted = journal;
    corrupted.free_heap ^= 0x40;
    CHECK(!crash_journal_is_valid(&corrupted), "flipped data byte invalidates");
    corrupted = journal;
    corrupted.checksum ^= 0x1;
    CHECK(!crash_journal_is_valid(&corrupted), "flipped checksum invalidates");
    corrupted = journal;
    corrupted.event_count = CRASH_JOURNAL_EVENT_COUNT + 1;
    CHECK(!crash_journal_is_valid(&corrupted), "out-of-range event count invalidates");
    corrupted = journal;
    corrupted.magic = 0;
    CHECK(!crash_journal_is_valid(&corrupted), "bad magic invalidates");

    printf("crash journal: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
