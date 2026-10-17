#include "mqtt_bridge.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "crash_journal.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "mqtt_events.h"
#include "mqtt_topics.h"
#include "version.h"

static const char *TAG = "mqtt";

#define MQTT_CONFIG_NAMESPACE "mqttcfg"
#define MQTT_KEY_PREFIX "prefix"
#define MQTT_KEY_URI "uri"
#define MQTT_KEY_USER "user"
#define MQTT_KEY_PASS "pass"

#define MQTT_SENSOR_INDEX 1

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static bool s_started;
static char s_prefix[80];

/* Captured once at start (before anything could change it) so every reconnect
 * republishes the same boot event. The crash journal is the previous session's
 * record; on a plain receiver no journal is written, so it is usually absent. */
static int s_reset_reason;
static bool s_journal_valid;
static crash_journal_t s_journal;

/* Publish the retained reboot event to <base>/event/reboot so Home Assistant
 * can show why the device (re)started. Retained, so a late subscriber still
 * sees the last boot; republished on every connect. */
static void mqtt_publish_reboot_event(const mqtt_topics_t *topics)
{
    char payload[512];
    if (mqtt_reboot_event_payload(s_reset_reason, fw_version(), s_journal_valid,
                                  &s_journal, payload, sizeof(payload)) != 0) {
        ESP_LOGW(TAG, "Could not build the reboot event payload");
        return;
    }
    esp_mqtt_client_publish(s_client, topics->event_reboot, payload, 0, 0, 1);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        ESP_LOGI(TAG, "connected to broker");
        mqtt_topics_t topics;
        if (mqtt_topics_build(s_prefix, &topics) != 0) {
            break;
        }
        /* Retained discovery hub so HA sees the device as soon as it is online. */
        const char *hub =
            "{\"name\":\"ESP32-IR\",\"unique_id\":\"esp32ir_recv\","
            "\"availability_topic\":\"%s\"}";
        char payload[320];
        snprintf(payload, sizeof(payload), hub, topics.status);
        esp_mqtt_client_publish(s_client, topics.state, payload, 0, 0, 1);

        char topic[MQTT_TOPIC_MAX];
        char value_payload[512];
        struct {
            const char *component;
            const char *name;
            const char *object_id;
            const char *state_topic;
            const char *unit;
            const char *device_class;
            const char *state_class;
        } entities[] = {
            {"sensor", "Leistung", "power", topics.power, "W", "power",
             "measurement"},
        };
        for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); ++i) {
            if (mqtt_discovery_topic(s_prefix, entities[i].component,
                                     "esp32ir_recv", entities[i].object_id,
                                     topic, sizeof(topic)) == 0 &&
                mqtt_discovery_payload(
                    entities[i].name, "esp32ir_recv_power",
                    entities[i].state_topic, entities[i].unit,
                    entities[i].device_class, entities[i].state_class,
                    topics.status, value_payload, sizeof(value_payload)) == 0) {
                esp_mqtt_client_publish(s_client, topic, value_payload, 0, 0, 1);
            }
        }
        mqtt_publish_reboot_event(&topics);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from broker");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error");
        break;
    default:
        break;
    }
}

bool mqtt_bridge_load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(MQTT_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    char uri[160] = {0};
    size_t uri_len = sizeof(uri);
    esp_err_t err = nvs_get_str(handle, MQTT_KEY_URI, uri, &uri_len);
    if (err != ESP_OK || uri[0] == '\0') {
        nvs_close(handle);
        return false;
    }
    size_t prefix_len = sizeof(s_prefix);
    if (nvs_get_str(handle, MQTT_KEY_PREFIX, s_prefix, &prefix_len) != ESP_OK ||
        s_prefix[0] == '\0') {
        snprintf(s_prefix, sizeof(s_prefix), "esp32ir/receiver");
    }
    nvs_close(handle);
    return true;
}

void mqtt_bridge_start(void)
{
    if (s_started) {
        return;
    }
    if (!mqtt_bridge_load_config()) {
        ESP_LOGI(TAG, "no broker configured; MQTT disabled");
        return;
    }

    mqtt_topics_t topics;
    if (mqtt_topics_build(s_prefix, &topics) != 0) {
        ESP_LOGE(TAG, "invalid MQTT prefix; MQTT disabled");
        return;
    }

    /* Snapshot the boot facts the reboot event reports. esp_reset_reason() is
     * stable for the whole session, and the crash journal (when present) is the
     * previous session's record. */
    s_reset_reason = (int)esp_reset_reason();
    bool journal_found = false;
    s_journal_valid = crash_journal_load(&s_journal, &journal_found) == ESP_OK &&
                      journal_found;

    nvs_handle_t handle;
    char uri[160] = {0};
    char user[64] = {0};
    char pass[96] = {0};
    if (nvs_open(MQTT_CONFIG_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(uri);
        (void)nvs_get_str(handle, MQTT_KEY_URI, uri, &len);
        len = sizeof(user);
        (void)nvs_get_str(handle, MQTT_KEY_USER, user, &len);
        len = sizeof(pass);
        (void)nvs_get_str(handle, MQTT_KEY_PASS, pass, &len);
        nvs_close(handle);
    }

    char lwt_payload[] = "offline";
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .session.last_will = {
            .topic = topics.status,
            .msg = lwt_payload,
            .msg_len = strlen(lwt_payload),
            .qos = 1,
            .retain = 1,
        },
    };
    if (user[0] != '\0') {
        cfg.credentials.username = user;
        cfg.credentials.authentication.password = pass;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "could not create MQTT client");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "could not start MQTT client");
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "MQTT client started for %s", uri);
}

void mqtt_bridge_publish_power(double watts)
{
    if (!s_connected) {
        return;
    }
    mqtt_topics_t topics;
    if (mqtt_topics_build(s_prefix, &topics) != 0) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddNumberToObject(root, "power", watts);
    cJSON_AddNumberToObject(root, "unit", 27);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json != NULL) {
        esp_mqtt_client_publish(s_client, topics.power, json, 0, 0, 1);
        cJSON_free(json);
    }
}

void mqtt_bridge_publish_history(const power_history_record_t *record)
{
    if (!s_connected || record == NULL) {
        return;
    }
    mqtt_topics_t topics;
    if (mqtt_topics_build(s_prefix, &topics) != 0) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddNumberToObject(root, "t", (double)record->interval_start_utc);
    cJSON_AddNumberToObject(root, "w", (double)record->average_milliwatts / 1000.0);
    cJSON_AddNumberToObject(root, "n", (double)record->sample_count);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json != NULL) {
        esp_mqtt_client_publish(s_client, topics.history, json, 0, 0, 0);
        cJSON_free(json);
    }
}

bool mqtt_bridge_started(void)
{
    return s_started;
}

void mqtt_bridge_status(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    if (!s_started) {
        snprintf(out, out_size, "mqtt: disabled (no broker configured)\n");
    } else {
        snprintf(out, out_size, "mqtt: %s, prefix=%s\n",
                 s_connected ? "connected" : "connecting", s_prefix);
    }
}
