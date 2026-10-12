#include "mqtt_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MQTT_HOST_TEST
/* glibc has no strlcpy; ESP-IDF (newlib) does. Provide it only for the host
 * test build so the production translation unit uses the platform version. */
static size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size != 0) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
#endif

/* JSON string escaping for the small, fixed set of characters a host name,
 * device name or unit can realistically contain. Anything below 0x20 and the
 * quote/backslash are escaped; the rest is copied verbatim (the strings are
 * UTF-8 from the configuration). */
static int json_escape(const char *in, char *out, size_t out_size)
{
    size_t written = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; ++p) {
        const char *escape = NULL;
        char buffer[7];
        if (*p == '"') {
            escape = "\\\"";
        } else if (*p == '\\') {
            escape = "\\\\";
        } else if (*p < 0x20) {
            snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned int)*p);
            escape = buffer;
        }
        if (escape != NULL) {
            size_t len = strlen(escape);
            if (written + len + 1 > out_size) {
                return -1;
            }
            memcpy(out + written, escape, len);
            written += len;
        } else {
            if (written + 2 > out_size) {
                return -1;
            }
            out[written++] = (char)*p;
        }
    }
    out[written] = '\0';
    return (int)written;
}

void mqtt_config_defaults(mqtt_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->port = MQTT_DEFAULT_PORT;
    strlcpy(config->base_topic, MQTT_DEFAULT_BASE_TOPIC, sizeof(config->base_topic));
}

esp_err_t mqtt_parse_host_port(const char *value, mqtt_config_t *config)
{
    if (value == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (*value == ' ' || *value == '\t') {
        ++value;
    }

    const char *colon = strrchr(value, ':');
    if (colon != NULL) {
        char host[MQTT_HOST_MAX_LEN + 1];
        size_t host_len = (size_t)(colon - value);
        if (host_len > MQTT_HOST_MAX_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(host, value, host_len);
        host[host_len] = '\0';

        char *end = NULL;
        long port = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || *end != '\0' || port <= 0 || port > 65535) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(config->host, host, sizeof(config->host));
        config->port = (uint16_t)port;
        return ESP_OK;
    }

    if (strlen(value) > MQTT_HOST_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(config->host, value, sizeof(config->host));
    return ESP_OK;
}

bool mqtt_config_is_valid(const mqtt_config_t *config)
{
    if (config == NULL) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    if (config->host[0] == '\0' || config->port == 0) {
        return false;
    }
    if (config->base_topic[0] == '\0') {
        return false;
    }
    for (const char *p = config->base_topic; *p != '\0'; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '/' || *p == '#') {
            return false;
        }
    }
    return true;
}

esp_err_t mqtt_build_topic(const mqtt_config_t *config, const char *suffix,
                           char *out, size_t out_size)
{
    if (config == NULL || suffix == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(out, out_size, "%s/%s", config->base_topic, suffix);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t mqtt_build_discovery_json(const mqtt_config_t *config,
                                    const mqtt_device_info_t *device,
                                    const char *object_id, const char *name,
                                    const char *unit, const char *device_class,
                                    const char *value_template,
                                    char *out, size_t out_size)
{
    if (config == NULL || device == NULL || object_id == NULL || name == NULL ||
        out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char topic[MQTT_TOPIC_MAX_LEN];
    if (mqtt_build_topic(config, "state", topic, sizeof(topic)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    char esc_name[128];
    char esc_id[96];
    char esc_dev_name[96];
    if (json_escape(name, esc_name, sizeof(esc_name)) < 0 ||
        json_escape(object_id, esc_id, sizeof(esc_id)) < 0 ||
        json_escape(device->device_name ? device->device_name : "esp32-ir",
                    esc_dev_name, sizeof(esc_dev_name)) < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *unit_part = "";
    char unit_buffer[64];
    if (unit != NULL && unit[0] != '\0') {
        char esc_unit[32];
        if (json_escape(unit, esc_unit, sizeof(esc_unit)) < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(unit_buffer, sizeof(unit_buffer), ",\"unit_of_measurement\":\"%s\"",
                 esc_unit);
        unit_part = unit_buffer;
    }

    const char *class_part = "";
    char class_buffer[64];
    if (device_class != NULL && device_class[0] != '\0') {
        char esc_class[32];
        if (json_escape(device_class, esc_class, sizeof(esc_class)) < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(class_buffer, sizeof(class_buffer), ",\"device_class\":\"%s\"",
                 esc_class);
        class_part = class_buffer;
    }

    int written = snprintf(
        out, out_size,
        "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"state_topic\":\"%s\","
        "\"value_template\":\"%s\"%s%s,"
        "\"availability_topic\":\"%s/status\",\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"model\":\"%s\","
        "\"manufacturer\":\"%s\",\"sw_version\":\"%s\"}}",
        esc_name, device->device_id, esc_id, topic, value_template,
        unit_part, class_part, config->base_topic,
        device->device_id, esc_dev_name,
        device->model ? device->model : "esp32-IR",
        device->manufacturer ? device->manufacturer : "esp32-IR",
        device->sw_version ? device->sw_version : "unknown");
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t mqtt_build_state_json(double power_w, bool power_valid,
                                double import_kwh, double export_kwh,
                                int64_t timestamp_utc, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written;
    if (power_valid) {
        written = snprintf(out, out_size,
                           "{\"power_w\":%.1f,\"import_kwh\":%.3f,"
                           "\"export_kwh\":%.3f,\"timestamp\":%lld}",
                           power_w, import_kwh, export_kwh,
                           (long long)timestamp_utc);
    } else {
        written = snprintf(out, out_size,
                           "{\"power_w\":null,\"import_kwh\":%.3f,"
                           "\"export_kwh\":%.3f,\"timestamp\":%lld}",
                           import_kwh, export_kwh, (long long)timestamp_utc);
    }
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
