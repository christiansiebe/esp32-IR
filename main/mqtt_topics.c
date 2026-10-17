#include "mqtt_topics.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Join <base>/<suffix> into out; returns 0/-1 on overflow. */
static int join(const char *base, const char *suffix, char *out, size_t n)
{
    int written = snprintf(out, n, "%s/%s", base, suffix);
    if (written < 0 || (size_t)written >= n) {
        if (n > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return 0;
}

static int prefix_valid(const char *prefix, char *trimmed, size_t n)
{
    if (prefix == NULL) {
        return -1;
    }
    while (*prefix == '/') {
        ++prefix;
    }
    size_t len = strlen(prefix);
    while (len > 0 && prefix[len - 1] == '/') {
        --len;
    }
    if (len == 0 || len >= n) {
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)prefix[i];
        if (c <= 0x20 || c == 0x7f) {
            return -1;
        }
    }
    memcpy(trimmed, prefix, len);
    trimmed[len] = '\0';
    return 0;
}

int mqtt_topics_build(const char *prefix, mqtt_topics_t *out)
{
    if (out == NULL) {
        return -1;
    }
    char base[80];
    if (prefix_valid(prefix, base, sizeof(base)) != 0) {
        return -1;
    }
    mqtt_topics_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.base, sizeof(tmp.base), "%s", base);
    if (join(base, "power", tmp.power, sizeof(tmp.power)) != 0 ||
        join(base, "history", tmp.history, sizeof(tmp.history)) != 0 ||
        join(base, "status", tmp.status, sizeof(tmp.status)) != 0 ||
        join(base, "state", tmp.state, sizeof(tmp.state)) != 0) {
        return -1;
    }
    *out = tmp;
    return 0;
}

/* Re-encode an OBIS text "A-B:C.D.E" to the "A-B:C.D.E*255" MQTT spelling. */
static int obis_topic_text(const char *obis_text, char *out, size_t n)
{
    int written = snprintf(out, n, "%s*255", obis_text);
    if (written < 0 || (size_t)written >= n) {
        return -1;
    }
    return 0;
}

int mqtt_topic_obis(const char *base, unsigned sensor, const char *obis_text,
                    char *out, size_t out_size)
{
    if (base == NULL || obis_text == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    char obis[32];
    if (obis_topic_text(obis_text, obis, sizeof(obis)) != 0) {
        return -1;
    }
    int written = snprintf(out, out_size, "%s/sensor/%u/obis/%s/value", base,
                           sensor, obis);
    if (written < 0 || (size_t)written >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return 0;
}

int mqtt_discovery_topic(const char *base, const char *component,
                         const char *node_id, const char *object_id, char *out,
                         size_t out_size)
{
    if (base == NULL || component == NULL || node_id == NULL ||
        object_id == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    int written = snprintf(out, out_size, "%s/homeassistant/%s/%s/%s/config",
                           base, component, node_id, object_id);
    if (written < 0 || (size_t)written >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return 0;
}

static int json_field(char *out, size_t n, size_t *pos, bool first,
                      const char *key, const char *value)
{
    int written = snprintf(out + *pos, n - *pos, "%s\"%s\":\"%s\"",
                           first ? "" : ",", key, value);
    if (written < 0 || (size_t)written >= n - *pos) {
        return -1;
    }
    *pos += (size_t)written;
    return 0;
}

int mqtt_discovery_payload(const char *name, const char *unique_id,
                           const char *state_topic, const char *unit,
                           const char *device_class, const char *state_class,
                           const char *availability_topic, char *out,
                           size_t out_size)
{
    if (name == NULL || unique_id == NULL || state_topic == NULL || out == NULL ||
        out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    size_t pos = 0;
    int written = snprintf(out, out_size, "{");
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }
    pos = (size_t)written;

    struct {
        const char *key;
        const char *value;
    } fields[] = {
        {"name", name},
        {"unique_id", unique_id},
        {"state_topic", state_topic},
        {"unit_of_measurement", unit},
        {"device_class", device_class},
        {"state_class", state_class},
        {"availability_topic", availability_topic},
    };
    bool first = true;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (fields[i].value == NULL) {
            continue;
        }
        if (json_field(out, out_size, &pos, first, fields[i].key,
                       fields[i].value) != 0) {
            out[0] = '\0';
            return -1;
        }
        first = false;
    }
    if (pos + 2 > out_size) {
        out[0] = '\0';
        return -1;
    }
    out[pos++] = '}';
    out[pos] = '\0';
    return 0;
}
