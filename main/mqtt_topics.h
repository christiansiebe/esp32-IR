#pragma once

/* Pure MQTT topic / Home Assistant discovery builders.
 *
 * Deliberately free of esp-mqtt and ESP-IDF: the string layout is the part
 * that breaks silently (wrong topic, malformed JSON), so it is unit-tested on
 * the host like boot_policy.c. The esp-mqtt glue stays a thin caller.
 *
 * Topic schema borrowed as a concept from mruettgers/SMLReader (GPL-3.0, no
 * code copied): one topic per OBIS code, plus fixed power/history/status
 * topics. Availability uses the MQTT LWT (last-will-and-testament) topic.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_TOPIC_MAX 160

typedef struct {
    char base[80];                /* sanitized prefix, e.g. "esp32ir/receiver" */
    char power[MQTT_TOPIC_MAX];   /* <base>/power, live power in W */
    char history[MQTT_TOPIC_MAX]; /* <base>/history, finished minute as JSON */
    char status[MQTT_TOPIC_MAX];  /* <base>/status, online/offline via LWT */
    char state[MQTT_TOPIC_MAX];   /* <base>/state, retained HA discovery hub */
    char event_reboot[MQTT_TOPIC_MAX]; /* <base>/event/reboot, retained reset reason */
} mqtt_topics_t;

/* Build the fixed topics. \p prefix is trimmed of surrounding '/' and must be
 * non-empty and free of spaces/control characters. Returns 0 on success, -1 on
 * an invalid prefix or output overflow (inputs untouched then). */
int mqtt_topics_build(const char *prefix, mqtt_topics_t *out);

/* Per-OBIS value topic: <base>/sensor/<sensor>/obis/<obis_text>/value, with the
 * OBIS text (e.g. "1-0:16.7.0") re-encoded to "1-0:16.7.0/255" like SMLReader.
 * Returns 0 on success, -1 on invalid input or overflow. */
int mqtt_topic_obis(const char *base, unsigned sensor, const char *obis_text,
                    char *out, size_t out_size);

/* Home Assistant discovery topic for one entity:
 * <base>/homeassistant/<component>/<node_id>/<object_id>/config
 * Returns 0 on success, -1 on invalid input or overflow. */
int mqtt_discovery_topic(const char *base, const char *component,
                         const char *node_id, const char *object_id, char *out,
                         size_t out_size);

/* HA discovery JSON payload. Only non-NULL optional fields are emitted.
 * Returns 0 on success, -1 on overflow (buffer left NUL-terminated). */
int mqtt_discovery_payload(const char *name, const char *unique_id,
                           const char *state_topic, const char *unit,
                           const char *device_class, const char *state_class,
                           const char *availability_topic, char *out,
                           size_t out_size);

#ifdef __cplusplus
}
#endif
