#include "mqtt_events.h"

#include <stdio.h>
#include <string.h>

#include "json_escape.h"

int mqtt_reboot_event_payload(int reason, const char *version,
                              bool journal_valid, const crash_journal_t *journal,
                              char *out, size_t out_size)
{
    if (version == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    /* The version comes from `git describe` and can carry a '-dirty' suffix or
     * an arbitrary tag text; escape it so a stray quote cannot break the JSON. */
    char version_json[64];
    if (json_escape(version, version_json, sizeof(version_json)) >=
        sizeof(version_json)) {
        return -1;
    }

    const char *reason_name = crash_journal_reset_reason_name(reason);
    bool deliberate = crash_journal_reset_is_deliberate(reason);

    int written;
    if (journal_valid && journal != NULL) {
        const char *journal_reason_name =
            crash_journal_reset_reason_name((int)journal->reset_reason);
        written = snprintf(
            out, out_size,
            "{\"event\":\"reboot\",\"reason\":%d,\"reason_name\":\"%s\","
            "\"version\":\"%s\",\"deliberate\":%s,"
            "\"journal\":{\"reset_reason\":%d,\"reset_reason_name\":\"%s\","
            "\"boot_count\":%lu,\"clean_shutdown\":%s,\"uptime_ms\":%llu}}",
            reason, reason_name, version_json, deliberate ? "true" : "false",
            (int)journal->reset_reason, journal_reason_name,
            (unsigned long)journal->boot_count,
            journal->clean_shutdown ? "true" : "false",
            (unsigned long long)journal->session_uptime_ms);
    } else {
        written = snprintf(out, out_size,
                           "{\"event\":\"reboot\",\"reason\":%d,"
                           "\"reason_name\":\"%s\",\"version\":\"%s\","
                           "\"deliberate\":%s}",
                           reason, reason_name, version_json,
                           deliberate ? "true" : "false");
    }
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}
