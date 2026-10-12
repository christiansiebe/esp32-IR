#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Pure, host-testable helpers for the optional MQTT bridge.
 *
 * The receiver can publish the latest meter values to an MQTT broker and
 * announce itself to Home Assistant via MQTT discovery. Everything here has no
 * ESP-IDF dependency (only stdio/stdlib/string) so the parsing, validation,
 * topic building and JSON generation are covered by the host tests. The
 * network/NVS side lives in mqtt_publish.c.
 */

#define MQTT_HOST_MAX_LEN   64
#define MQTT_USER_MAX_LEN   32
#define MQTT_PASS_MAX_LEN   64
#define MQTT_BASE_TOPIC_MAX_LEN 48
#define MQTT_TOPIC_MAX_LEN  160

#define MQTT_DEFAULT_PORT      1883u
#define MQTT_DEFAULT_BASE_TOPIC "esp32ir"

typedef struct {
    bool enabled;
    char host[MQTT_HOST_MAX_LEN + 1];
    uint16_t port;
    char username[MQTT_USER_MAX_LEN + 1];
    char password[MQTT_PASS_MAX_LEN + 1];
    char base_topic[MQTT_BASE_TOPIC_MAX_LEN + 1];
} mqtt_config_t;

typedef struct {
    const char *device_id;    /* stable id, e.g. the receiver MAC */
    const char *device_name;  /* friendly name shown in Home Assistant */
    const char *model;
    const char *manufacturer;
    const char *sw_version;
} mqtt_device_info_t;

/* Fill a config with the defaults (disabled, port 1883, base topic "esp32ir"). */
void mqtt_config_defaults(mqtt_config_t *config);

/* Parse "host" or "host:port" into config (port keeps its previous value when
 * omitted). Empty host clears the config. Returns ESP_ERR_INVALID_ARG on a
 * malformed host/port. */
esp_err_t mqtt_parse_host_port(const char *value, mqtt_config_t *config);

/* A config is usable when disabled, or enabled with a non-empty host and a
 * non-zero port and a non-empty base topic without whitespace or '/'. */
bool mqtt_config_is_valid(const mqtt_config_t *config);

/* Build "<base_topic>/<suffix>". Returns ESP_ERR_INVALID_ARG on overflow. */
esp_err_t mqtt_build_topic(const mqtt_config_t *config, const char *suffix,
                           char *out, size_t out_size);

/* Build the Home Assistant discovery payload for one sensor. */
esp_err_t mqtt_build_discovery_json(const mqtt_config_t *config,
                                    const mqtt_device_info_t *device,
                                    const char *object_id, const char *name,
                                    const char *unit, const char *device_class,
                                    const char *value_template,
                                    char *out, size_t out_size);

/* Build the state payload published on "<base_topic>/state". */
esp_err_t mqtt_build_state_json(double power_w, bool power_valid,
                                double import_kwh, double export_kwh,
                                int64_t timestamp_utc, char *out, size_t out_size);
