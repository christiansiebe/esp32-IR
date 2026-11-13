#include "mqtt_bridge.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "crash_journal.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "mqtt_cmd.h"
#include "mqtt_events.h"
#include "mqtt_publish.h"
#include "mqtt_topics.h"
#include "net_config.h"
#include "version.h"
#include "web_console.h"

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

/* Remote command channel (<base>/cmd). Reassembly runs on the MQTT task, but the
 * command itself is handed to a separate worker: a slow command (e.g. 'pull')
 * must not block the MQTT task's keepalive handling. */
static mqtt_cmd_rx_t s_cmd_rx;
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_cmd_task;
static char s_cmd_scratch[MQTT_CMD_LINE_MAX];

/* Run one command line through the same dispatcher as the web/serial console and
 * publish the answer retained to <base>/cmd/result. Runs on the cmd worker. */
static void mqtt_cmd_run_and_publish(const char *line)
{
    char out[WEB_CONSOLE_OUT_MAX];
    out[0] = '\0';
    bool known = web_console_run(line, out, sizeof(out));

    if (!s_connected) {
        return;
    }
    mqtt_topics_t topics;
    if (mqtt_topics_build(s_prefix, &topics) != 0) {
        return;
    }
    char payload[MQTT_CMD_PAYLOAD_MAX];
    if (mqtt_cmd_result_payload(line, known, out, payload, sizeof(payload)) != 0) {
        ESP_LOGW(TAG, "command result too large to publish");
        return;
    }
    esp_mqtt_client_publish(s_client, topics.cmd_result, payload, 0, 0, 1);
}

static void mqtt_cmd_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, s_cmd_scratch, portMAX_DELAY) == pdTRUE) {
            mqtt_cmd_run_and_publish(s_cmd_scratch);
        }
    }
}

/* Create the command queue and worker once. Idempotent: set_config restarts the
 * client but must not spawn a second worker. */
static void mqtt_cmd_worker_ensure(void)
{
    if (s_cmd_queue == NULL) {
        s_cmd_queue = xQueueCreate(1, MQTT_CMD_LINE_MAX);
        if (s_cmd_queue == NULL) {
            ESP_LOGW(TAG, "could not create the command queue");
            return;
        }
    }
    if (s_cmd_task == NULL) {
        if (xTaskCreate(mqtt_cmd_worker, "mqtt_cmd", 8192, NULL, 4,
                        &s_cmd_task) != pdPASS) {
            s_cmd_task = NULL;
            ESP_LOGW(TAG, "could not start the command worker");
        }
    }
}

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

        char import_topic[MQTT_TOPIC_MAX];
        char export_topic[MQTT_TOPIC_MAX];
        snprintf(import_topic, sizeof(import_topic), "%s/import", topics.base);
        snprintf(export_topic, sizeof(export_topic), "%s/export", topics.base);

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
            /* Cumulative counters: Home Assistant's statistics engine records
             * the minute/hourly history itself from these two total_increasing
             * sensors, so the device no longer has to be polled for old data. */
            {"sensor", "Bezug gesamt", "import", import_topic, "kWh", "energy",
             "total_increasing"},
            {"sensor", "Einspeisung gesamt", "export", export_topic, "kWh",
             "energy", "total_increasing"},
        };
        for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); ++i) {
            char unique_id[96];
            snprintf(unique_id, sizeof(unique_id), "esp32ir_recv_%s",
                     entities[i].object_id);
            if (mqtt_discovery_topic(s_prefix, entities[i].component,
                                     "esp32ir_recv", entities[i].object_id,
                                     topic, sizeof(topic)) == 0 &&
                mqtt_discovery_payload(
                    entities[i].name, unique_id,
                    entities[i].state_topic, entities[i].unit,
                    entities[i].device_class, entities[i].state_class,
                    topics.status, value_payload, sizeof(value_payload)) == 0) {
                esp_mqtt_client_publish(s_client, topic, value_payload, 0, 0, 1);
            }
        }
        mqtt_publish_reboot_event(&topics);

        /* Subscribe last: the command channel is optional and must not delay the
         * discovery/reboot publishes that Home Assistant waits for. QoS 1 so a
         * command is not silently lost; the broker redelivers on reconnect. */
        mqtt_cmd_rx_reset(&s_cmd_rx);
        esp_mqtt_client_subscribe(s_client, topics.cmd, 1);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        mqtt_cmd_rx_reset(&s_cmd_rx);
        ESP_LOGW(TAG, "disconnected from broker");
        break;
    case MQTT_EVENT_DATA: {
        esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
        if (event == NULL) {
            break;
        }
        /* Only the command topic is subscribed, but esp-mqtt leaves topic NULL on
         * continuation chunks unless CONFIG_MQTT_TOPIC_PRESENT_ALL_DATA_EVENTS is
         * set. So: check the topic when it is present, and otherwise continue an
         * already-started reassembly. Anything else is ignored. */
        if (event->topic != NULL) {
            mqtt_topics_t topics;
            if (mqtt_topics_build(s_prefix, &topics) != 0 ||
                (int)strlen(topics.cmd) != event->topic_len ||
                strncmp(topics.cmd, event->topic, (size_t)event->topic_len) != 0) {
                break;
            }
        } else if (!s_cmd_rx.active) {
            break;
        }
        mqtt_cmd_rx_result_t result =
            mqtt_cmd_rx_feed(&s_cmd_rx, event->data, event->data_len,
                             event->current_data_offset, event->total_data_len);
        if (result == MQTT_CMD_RX_DONE && s_cmd_queue != NULL) {
            char line[MQTT_CMD_LINE_MAX];
            strlcpy(line, mqtt_cmd_rx_line(&s_cmd_rx), sizeof(line));
            /* The queue holds one command: if the worker is still busy the new
             * one is rejected (never queued twice), so a burst cannot pile up. */
            if (xQueueSend(s_cmd_queue, line, 0) != pdTRUE) {
                ESP_LOGW(TAG, "command worker busy; dropping a command");
            }
        }
        break;
    }
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
    char uri[NET_CONFIG_MQTT_URI_MAX] = {0};
    char user[NET_CONFIG_MQTT_USER_MAX + 1] = {0};
    char pass[NET_CONFIG_MQTT_PASS_MAX + 1] = {0};
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
    mqtt_cmd_worker_ensure();
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
    /* A bare number, not JSON: the discovery entity declares <base>/power as its
     * state_topic with no value_template, so a JSON object would read as
     * "unknown" in Home Assistant. */
    char payload[24];
    if (mqtt_power_payload(watts, payload, sizeof(payload)) == 0) {
        esp_mqtt_client_publish(s_client, topics.power, payload, 0, 0, 1);
    }
}

/* Publish one cumulative counter (import/export) as a retained bare number. */
static void publish_energy(const char *suffix, double raw, int8_t scaler)
{
    if (!s_connected) {
        return;
    }
    mqtt_topics_t topics;
    if (mqtt_topics_build(s_prefix, &topics) != 0) {
        return;
    }
    char topic[MQTT_TOPIC_MAX];
    char payload[24];
    if (snprintf(topic, sizeof(topic), "%s/%s", topics.base, suffix) >=
        (int)sizeof(topic)) {
        return;
    }
    if (mqtt_energy_payload(mqtt_energy_to_kwh(raw, scaler), payload,
                            sizeof(payload)) == 0) {
        esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
    }
}

void mqtt_bridge_publish_import(double raw, int8_t scaler)
{
    publish_energy("import", raw, scaler);
}

void mqtt_bridge_publish_export(double raw, int8_t scaler)
{
    publish_energy("export", raw, scaler);
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

/* Tear the running client down so the next mqtt_bridge_start() builds it from
 * the (new) NVS values. A no-op when nothing is running. */
static void mqtt_bridge_stop(void)
{
    if (s_client != NULL) {
        (void)esp_mqtt_client_stop(s_client);
        (void)esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    s_started = false;
}

bool mqtt_bridge_set_config(const char *key, const char *value, char *err,
                            size_t err_size)
{
    if (key == NULL || value == NULL) {
        snprintf(err, err_size, "mqtt: missing key or value\n");
        return false;
    }

    const char *nvs_key = NULL;
    const char *nvs_value = value;
    char prefix_buf[NET_CONFIG_MQTT_PREFIX_MAX] = {0};
    if (strcmp(key, "mqtt-uri") == 0) {
        if (!net_config_validate_mqtt_uri(value, err, err_size)) {
            return false;
        }
        nvs_key = MQTT_KEY_URI;
    } else if (strcmp(key, "mqtt-prefix") == 0) {
        /* Persist the trimmed form: the loader stores it in an 80-byte buffer
         * and mqtt_topics_build trims again, so a raw value that only fits after
         * trimming would otherwise be silently dropped on the next boot. */
        if (!net_config_normalize_mqtt_prefix(value, prefix_buf, sizeof(prefix_buf),
                                              err, err_size)) {
            return false;
        }
        nvs_key = MQTT_KEY_PREFIX;
        nvs_value = prefix_buf;
    } else if (strcmp(key, "mqtt-user") == 0) {
        if (!net_config_validate_mqtt_credential(value, NET_CONFIG_MQTT_USER_MAX,
                                                 "mqtt-user", err, err_size)) {
            return false;
        }
        nvs_key = MQTT_KEY_USER;
    } else if (strcmp(key, "mqtt-pass") == 0) {
        if (!net_config_validate_mqtt_credential(value, NET_CONFIG_MQTT_PASS_MAX,
                                                 "mqtt-pass", err, err_size)) {
            return false;
        }
        nvs_key = MQTT_KEY_PASS;
    } else {
        snprintf(err, err_size, "mqtt: unknown key '%s'\n", key);
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(MQTT_CONFIG_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        snprintf(err, err_size, "mqtt: could not open NVS\n");
        return false;
    }
    esp_err_t nvs_err = nvs_set_str(handle, nvs_key, nvs_value);
    if (nvs_err == ESP_OK) {
        nvs_err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (nvs_err != ESP_OK) {
        snprintf(err, err_size, "mqtt: could not persist %s\n", key);
        return false;
    }

    /* Rebuild the client so the new broker/credentials/prefix take effect. A
     * URI that is now absent or invalid simply leaves MQTT disabled. */
    mqtt_bridge_stop();
    mqtt_bridge_start();
    snprintf(err, err_size, "mqtt %s updated\n", key);
    return true;
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
