/* Host tests for the pure MQTT bridge helpers (config parsing, topic building,
 * Home Assistant discovery and state JSON). No network, no ESP-IDF. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_bridge.h"

static int g_checks;
static int g_failures;

/* glibc lacks strlcpy; provide it for the host test. */
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

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

static void test_defaults(void)
{
    mqtt_config_t config;
    mqtt_config_defaults(&config);
    CHECK(!config.enabled, "defaults are disabled");
    CHECK(config.port == 1883, "default port is 1883");
    CHECK(strcmp(config.base_topic, "esp32ir") == 0, "default base topic");
    CHECK(mqtt_config_is_valid(&config), "disabled config is valid");
}

static void test_parse_host_only(void)
{
    mqtt_config_t config;
    mqtt_config_defaults(&config);
    CHECK(mqtt_parse_host_port("broker.local", &config) == ESP_OK, "host parsed");
    CHECK(strcmp(config.host, "broker.local") == 0, "host stored");
    CHECK(config.port == 1883, "port keeps default when omitted");
}

static void test_parse_host_and_port(void)
{
    mqtt_config_t config;
    mqtt_config_defaults(&config);
    CHECK(mqtt_parse_host_port("192.168.1.10:8883", &config) == ESP_OK, "host:port parsed");
    CHECK(strcmp(config.host, "192.168.1.10") == 0, "host part");
    CHECK(config.port == 8883, "port part");
}

static void test_parse_rejects_bad_port(void)
{
    mqtt_config_t config;
    mqtt_config_defaults(&config);
    CHECK(mqtt_parse_host_port("broker:0", &config) != ESP_OK, "port 0 rejected");
    CHECK(mqtt_parse_host_port("broker:99999", &config) != ESP_OK, "port > 65535 rejected");
    CHECK(mqtt_parse_host_port("broker:abc", &config) != ESP_OK, "non-numeric port rejected");
}

static void test_validity(void)
{
    mqtt_config_t config;
    mqtt_config_defaults(&config);
    config.enabled = true;
    CHECK(!mqtt_config_is_valid(&config), "enabled without host is invalid");

    strlcpy(config.host, "broker", sizeof(config.host));
    CHECK(mqtt_config_is_valid(&config), "enabled with host is valid");

    strlcpy(config.base_topic, "bad/topic", sizeof(config.base_topic));
    CHECK(!mqtt_config_is_valid(&config), "base topic with '/' is invalid");

    strlcpy(config.base_topic, "esp32ir", sizeof(config.base_topic));
    config.port = 0;
    CHECK(!mqtt_config_is_valid(&config), "port 0 is invalid");
}

static void test_topics(void)
{
    mqtt_config_t config;
    mqtt_config_defaults(&config);
    strlcpy(config.base_topic, "home/esp32ir", sizeof(config.base_topic));
    /* '/' is rejected by validity, but topic building still joins verbatim. */
    char topic[MQTT_TOPIC_MAX_LEN];
    CHECK(mqtt_build_topic(&config, "state", topic, sizeof(topic)) == ESP_OK, "topic built");
    CHECK(strcmp(topic, "home/esp32ir/state") == 0, "topic content");

    char small[4];
    CHECK(mqtt_build_topic(&config, "state", small, sizeof(small)) != ESP_OK,
          "overflowing topic rejected");
}

static void test_discovery_json(void)
{
    mqtt_config_t config;
    mqtt_config_defaults(&config);
    strlcpy(config.host, "broker", sizeof(config.host));
    config.enabled = true;

    mqtt_device_info_t device = {
        .device_id = "AABBCCDDEEFF",
        .device_name = "ESP32-IR Meter",
        .model = "esp32-IR",
        .manufacturer = "esp32-IR",
        .sw_version = "1.0.0",
    };

    char json[512];
    CHECK(mqtt_build_discovery_json(&config, &device, "power", "Power", "W",
                                    "power", "{{ value_json.power_w }}",
                                    json, sizeof(json)) == ESP_OK, "discovery built");
    CHECK(strstr(json, "\"unique_id\":\"AABBCCDDEEFF_power\"") != NULL, "unique id");
    CHECK(strstr(json, "\"state_topic\":\"esp32ir/state\"") != NULL, "state topic");
    CHECK(strstr(json, "\"unit_of_measurement\":\"W\"") != NULL, "unit");
    CHECK(strstr(json, "\"device_class\":\"power\"") != NULL, "device class");
    CHECK(strstr(json, "\"value_template\":\"{{ value_json.power_w }}\"") != NULL,
          "value template");
    CHECK(strstr(json, "\"availability_topic\":\"esp32ir/status\"") != NULL,
          "availability topic");
    CHECK(strstr(json, "\"identifiers\":[\"AABBCCDDEEFF\"]") != NULL, "device id");

    /* A name with a quote must be escaped, not break the JSON. */
    CHECK(mqtt_build_discovery_json(&config, &device, "power", "Po\"wer", "W",
                                    NULL, "{{ value_json.power_w }}",
                                    json, sizeof(json)) == ESP_OK, "escaping name built");
    CHECK(strstr(json, "Po\\\"wer") != NULL, "quote escaped");
}

static void test_state_json(void)
{
    char json[256];
    CHECK(mqtt_build_state_json(1234.5, true, 42.5, 1.25, 1700000000,
                                json, sizeof(json)) == ESP_OK, "state built");
    CHECK(strcmp(json,
                 "{\"power_w\":1234.5,\"import_kwh\":42.500,\"export_kwh\":1.250,"
                 "\"timestamp\":1700000000}") == 0, "state content");

    CHECK(mqtt_build_state_json(0.0, false, 42.5, 1.25, 1700000000,
                                json, sizeof(json)) == ESP_OK, "invalid power state built");
    CHECK(strstr(json, "\"power_w\":null") != NULL, "null power when invalid");
}

int main(void)
{
    test_defaults();
    test_parse_host_only();
    test_parse_host_and_port();
    test_parse_rejects_bad_port();
    test_validity();
    test_topics();
    test_discovery_json();
    test_state_json();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
