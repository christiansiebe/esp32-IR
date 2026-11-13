#include "mqtt_cmd.h"

#include <stdio.h>
#include <string.h>

#include "json_escape.h"

void mqtt_cmd_rx_reset(mqtt_cmd_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }
    rx->line[0] = '\0';
    rx->len = 0;
    rx->expected = 0;
    rx->active = false;
}

mqtt_cmd_rx_result_t mqtt_cmd_rx_feed(mqtt_cmd_rx_t *rx, const char *data,
                                      int data_len, int offset, int total_len)
{
    if (rx == NULL || data == NULL || data_len < 0 || offset < 0 ||
        total_len <= 0) {
        if (rx != NULL) {
            mqtt_cmd_rx_reset(rx);
        }
        return MQTT_CMD_RX_ERROR;
    }
    /* The line buffer holds total_len bytes plus the terminator. */
    if ((size_t)total_len >= sizeof(rx->line)) {
        mqtt_cmd_rx_reset(rx);
        return MQTT_CMD_RX_ERROR;
    }

    if (offset == 0) {
        /* First chunk of a message (also the whole message for a short one). */
        rx->active = true;
        rx->len = 0;
        rx->expected = total_len;
    } else if (!rx->active || (size_t)offset != rx->len) {
        /* A gap or a chunk without a start: drop and resync on the next start. */
        mqtt_cmd_rx_reset(rx);
        return MQTT_CMD_RX_ERROR;
    }

    if ((size_t)offset + (size_t)data_len > (size_t)rx->expected) {
        mqtt_cmd_rx_reset(rx);
        return MQTT_CMD_RX_ERROR;
    }

    memcpy(rx->line + offset, data, (size_t)data_len);
    rx->len = (size_t)offset + (size_t)data_len;

    if (rx->len < (size_t)rx->expected) {
        return MQTT_CMD_RX_INCOMPLETE;
    }

    rx->line[rx->len] = '\0';
    rx->active = false;
    return MQTT_CMD_RX_DONE;
}

const char *mqtt_cmd_rx_line(const mqtt_cmd_rx_t *rx)
{
    if (rx == NULL) {
        return "";
    }
    return rx->line;
}

int mqtt_cmd_result_payload(const char *line, bool ok, const char *out, char *dst,
                            size_t dst_size)
{
    if (line == NULL || out == NULL || dst == NULL || dst_size == 0) {
        return -1;
    }
    dst[0] = '\0';

    /* Room for the escapes, minus the fixed JSON scaffolding. json_escape
     * truncates safely, so a very long 'out' still yields valid JSON. */
    char esc_line[MQTT_CMD_LINE_MAX];
    char esc_out[MQTT_CMD_PAYLOAD_MAX / 2];
    json_escape(line, esc_line, sizeof(esc_line));
    json_escape(out, esc_out, sizeof(esc_out));

    int written = snprintf(dst, dst_size, "{\"cmd\":\"%s\",\"ok\":%s,\"out\":\"%s\"}",
                           esc_line, ok ? "true" : "false", esc_out);
    if (written < 0 || (size_t)written >= dst_size) {
        dst[0] = '\0';
        return -1;
    }
    return 0;
}
