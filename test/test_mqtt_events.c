/* Host tests for the pure MQTT reboot-event payload builder.
 *
 * The reset-reason mapping is single-sourced in crash_journal.c, so these tests
 * also pin the mapping the boot log and the MQTT event share. The payload is
 * what Home Assistant parses, so the JSON shape (and the optional journal
 * summary) must stay exactly as advertised. */
#include <stdio.h>
#include <string.h>

#include "crash_journal.h"
#include "mqtt_events.h"

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

static void test_reason_names(void)
{
    CHECK(strcmp(crash_journal_reset_reason_name(1), "power-on") == 0,
          "power-on name");
    CHECK(strcmp(crash_journal_reset_reason_name(3), "software-restart") == 0,
          "software-restart name");
    CHECK(strcmp(crash_journal_reset_reason_name(4), "panic") == 0,
          "panic name");
    CHECK(strcmp(crash_journal_reset_reason_name(6), "task-watchdog") == 0,
          "task-watchdog name");
    CHECK(strcmp(crash_journal_reset_reason_name(9), "brownout") == 0,
          "brownout name");
    CHECK(strcmp(crash_journal_reset_reason_name(999), "other") == 0,
          "unknown reason falls back to other");
}

static void test_minimal_payload(void)
{
    char out[512];
    CHECK(mqtt_reboot_event_payload(4 /* panic */, "v0.2.0", false, NULL, out,
                                    sizeof(out)) == 0,
          "minimal payload ok");
    CHECK(strstr(out, "\"event\":\"reboot\"") != NULL, "event field");
    CHECK(strstr(out, "\"reason\":4") != NULL, "reason code");
    CHECK(strstr(out, "\"reason_name\":\"panic\"") != NULL, "reason name");
    CHECK(strstr(out, "\"version\":\"v0.2.0\"") != NULL, "version field");
    CHECK(strstr(out, "\"deliberate\":false") != NULL, "panic not deliberate");
    CHECK(strstr(out, "journal") == NULL, "no journal summary when absent");
    CHECK(out[0] == '{' && out[strlen(out) - 1] == '}',
          "payload is a JSON object");
    CHECK(strstr(out, ",,") == NULL, "no empty JSON field");
}

static void test_deliberate_flag(void)
{
    char out[512];
    CHECK(mqtt_reboot_event_payload(3 /* software-restart */, "v0.2.0", false,
                                    NULL, out, sizeof(out)) == 0,
          "software restart payload ok");
    CHECK(strstr(out, "\"deliberate\":true") != NULL,
          "software restart is deliberate");
}

static void test_journal_summary(void)
{
    crash_journal_t journal;
    crash_journal_make_fresh(&journal, 17, 4 /* panic */);
    journal.session_uptime_ms = 123456;
    journal.clean_shutdown = 0;
    crash_journal_finalize(&journal);
    CHECK(crash_journal_is_valid(&journal), "fixture journal valid");

    char out[512];
    CHECK(mqtt_reboot_event_payload(1 /* power-on */, "v0.2.0", true, &journal,
                                    out, sizeof(out)) == 0,
          "payload with journal ok");
    CHECK(strstr(out, "\"journal\":{") != NULL, "journal object present");
    CHECK(strstr(out, "\"reset_reason\":4") != NULL, "journal reset reason");
    CHECK(strstr(out, "\"reset_reason_name\":\"panic\"") != NULL,
          "journal reset reason name");
    CHECK(strstr(out, "\"boot_count\":17") != NULL, "journal boot count");
    CHECK(strstr(out, "\"clean_shutdown\":false") != NULL,
          "journal clean shutdown");
    CHECK(strstr(out, "\"uptime_ms\":123456") != NULL, "journal uptime");
    CHECK(out[strlen(out) - 1] == '}', "closing brace");

    /* clean_shutdown flips the summary flag without touching the reason. */
    journal.clean_shutdown = 1;
    crash_journal_finalize(&journal);
    CHECK(mqtt_reboot_event_payload(1, "v0.2.0", true, &journal, out,
                                    sizeof(out)) == 0,
          "clean payload ok");
    CHECK(strstr(out, "\"clean_shutdown\":true") != NULL,
          "clean shutdown reported");
}

static void test_version_escaping(void)
{
    char out[512];
    CHECK(mqtt_reboot_event_payload(1, "v1.0.0-3-gabc\"1234", false, NULL, out,
                                    sizeof(out)) == 0,
          "quote in version escaped, still valid");
    CHECK(strstr(out, "\"version\":\"v1.0.0-3-gabc\\\"1234\"") != NULL,
          "escaped quote in version");

    CHECK(mqtt_reboot_event_payload(1, NULL, false, NULL, out, sizeof(out)) == -1,
          "NULL version rejected");
}

static void test_overflow(void)
{
    char small[16];
    CHECK(mqtt_reboot_event_payload(4, "v0.2.0", false, NULL, small,
                                    sizeof(small)) == -1,
          "overflow rejected");
    CHECK(small[0] == '\0', "overflow leaves buffer empty");
}

int main(void)
{
    test_reason_names();
    test_minimal_payload();
    test_deliberate_flag();
    test_journal_summary();
    test_version_escaping();
    test_overflow();
    printf("mqtt events: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
