/* Receiver-side MQTT service events (pure helpers, host-tested).
 *
 * The reboot event is the MQTT counterpart of the crash journal: on every boot
 * the receiver publishes a retained message that tells Home Assistant why the
 * device restarted (reset reason + firmware version, plus the stored journal
 * summary when one exists). Keeping the payload builder free of esp-mqtt lets
 * the wire layout be unit-tested on the host, like mqtt_topics.c.
 *
 * The reset-reason-to-string mapping is single-sourced in crash_journal.c
 * (crash_journal_reset_reason_name), so the MQTT event and the boot log can
 * never disagree about what a reset reason means.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "crash_journal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the retained payload published to <base>/event/reboot:
 *
 *   {"event":"reboot","reason":<code>,"reason_name":"<name>",
 *    "version":"<fw>","deliberate":<true|false>}
 *
 * When \p journal_valid is true a summary of the previous session's crash
 * journal is appended:
 *
 *   "journal":{"reset_reason":<code>,"reset_reason_name":"<name>",
 *              "boot_count":<n>,"clean_shutdown":<bool>,"uptime_ms":<n>}
 *
 * Returns 0 on success, -1 on invalid arguments or output overflow (the buffer
 * is left empty then). */
int mqtt_reboot_event_payload(int reason, const char *version,
                              bool journal_valid, const crash_journal_t *journal,
                              char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
