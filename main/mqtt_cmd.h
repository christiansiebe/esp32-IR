#pragma once

/* Remote command channel over MQTT (pure, host-tested).
 *
 * The receiver already publishes state to MQTT. This module adds the reverse
 * direction: a line published to <base>/cmd is fed through the same command
 * dispatcher as the web/serial console (web_console_run) and the answer is
 * published retained to <base>/cmd/result.
 *
 * Two parts live here, deliberately free of esp-mqtt and ESP-IDF so they can be
 * unit-tested on the host:
 *   1. reassembly of a payload that esp-mqtt may deliver in several
 *      MQTT_EVENT_DATA events (topic/cmd, current_data_offset, total_data_len);
 *   2. the JSON result payload builder.
 * The esp-mqtt glue (subscribe, publish, worker task) stays in mqtt_bridge.c.
 *
 * The MQTT task must never run a command itself: a slow command (e.g. 'pull')
 * would stall the keepalive and drop the broker connection. mqtt_bridge.c hands
 * the finished line to a dedicated worker task instead.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest accepted command line, matching web_console's own line limit. A line
 * that does not fit is rejected rather than truncated: a cut command could run
 * something other than what was sent. */
#define MQTT_CMD_LINE_MAX 192

/* Upper bound for the result JSON. Kept under the default esp-mqtt TX buffer
 * (MQTT_BUFFER_SIZE_BYTE, 1024) so one publish always fits. */
#define MQTT_CMD_PAYLOAD_MAX 1024

typedef enum {
    MQTT_CMD_RX_INCOMPLETE = 0, /* more events needed for this message */
    MQTT_CMD_RX_DONE,           /* the line is complete and NUL-terminated */
    MQTT_CMD_RX_ERROR,          /* malformed framing or too long */
} mqtt_cmd_rx_result_t;

typedef struct {
    char line[MQTT_CMD_LINE_MAX]; /* assembled command line (NUL-terminated) */
    size_t len;                   /* bytes received so far */
    int expected;                 /* total_data_len of the current message */
    bool active;                  /* a message is being reassembled */
} mqtt_cmd_rx_t;

/* Clear the reassembly state (call on connect/disconnect). */
void mqtt_cmd_rx_reset(mqtt_cmd_rx_t *rx);

/* Feed one MQTT_EVENT_DATA chunk. \p offset is current_data_offset and
 * \p total_len is total_data_len. Returns DONE once the whole line is in
 * rx->line; INCOMPLETE while more chunks are expected; ERROR on a gap, an
 * out-of-range offset or a line longer than MQTT_CMD_LINE_MAX - 1. On ERROR the
 * state is reset, so the next message starts clean. */
mqtt_cmd_rx_result_t mqtt_cmd_rx_feed(mqtt_cmd_rx_t *rx, const char *data,
                                      int data_len, int offset, int total_len);

/* The completed command line (never NULL; empty before the first DONE). */
const char *mqtt_cmd_rx_line(const mqtt_cmd_rx_t *rx);

/* Build {"cmd":"<line>","ok":<bool>,"out":"<output>"} into dst. Both strings
 * are JSON-escaped and truncated to fit. Returns 0 on success, -1 on invalid
 * arguments or if even the fixed part does not fit (dst left empty then). */
int mqtt_cmd_result_payload(const char *line, bool ok, const char *out, char *dst,
                            size_t dst_size);

#ifdef __cplusplus
}
#endif
