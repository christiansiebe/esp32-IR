#include "mqtt_publish.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "mqtt_client.h" /* ESP-IDF esp_mqtt_client_* */
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MQTT_NAMESPACE     "mqttcfg"
#define MQTT_KEY_ENABLED   "enabled"
#define MQTT_KEY_HOST      "host"
#define MQTT_KEY_PORT      "port"
#define MQTT_KEY_USER      "user"
#define MQTT_KEY_PASS      "pass"
#define MQTT_KEY_TOPIC     "topic"

#define MQTT_TASK_STACK    4096u
#define MQTT_TASK_PRIORITY 3u
#define MQTT_TICK_MS       30000u

static const char *TAG = "mqtt";

static mqtt_config_t g_config;
static esp_mqtt_client_handle_t g_client;
static volatile bool g_connected;
static volatile bool g_started;
static portMUX_TYPE g_value_lock = portMUX_INITIALIZER_UNLOCKED;
static double g_power_w;
static bool g_power_valid;
static double g_import_kwh;
static double g_export_kwh;

static void load_config(void)
{
    mqtt_config_defaults(&g_config);
    nvs_handle_t handle;
    if (nvs_open(MQTT_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t enabled = 0;
    nvs_get_u8(handle, MQTT_KEY_ENABLED, &enabled);
    g_config.enabled = enabled != 0;
    uint16_t port = 0;
    if (nvs_get_u16(handle, MQTT_KEY_PORT, &port) == ESP_OK && port != 0) {
        g_config.port = port;
    }
    size_t len = sizeof(g_config.host);
    nvs_get_str(handle, MQTT_KEY_HOST, g_config.host, &len);
    len = sizeof(g_config.username);
    nvs_get_str(handle, MQTT_KEY_USER, g_config.username, &len);
    len = sizeof(g_config.password);
    nvs_get_str(handle, MQTT_KEY_PASS, g_config.password, &len);
    len = sizeof(g_config.base_topic);
    if (nvs_get_str(handle, MQTT_KEY_TOPIC, g_config.base_topic, &len) != ESP_OK ||
        g_config.base_topic[0] == '\0') {
        strlcpy(g_config.base_topic, MQTT_DEFAULT_BASE_TOPIC,
                sizeof(g_config.base_topic));
    }
    nvs_close(handle);
}

static esp_err_t save_config(const mqtt_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, MQTT_KEY_ENABLED, config->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_KEY_HOST, config->host);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, MQTT_KEY_PORT, config->port);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_KEY_USER, config->username);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_KEY_PASS, config->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_KEY_TOPIC, config->base_topic);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

void mqtt_publish_get_config(mqtt_config_t *config)
{
    if (config != NULL) {
        *config = g_config;
    }
}

static void device_id_string(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        strlcpy(out, "esp32ir", out_size);
        return;
    }
    snprintf(out, out_size, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void publish_discovery(void)
{
    char device_id[16];
    device_id_string(device_id, sizeof(device_id));

    mqtt_device_info_t device = {
        .device_id = device_id,
        .device_name = "ESP32-IR Meter",
        .model = "esp32-IR",
        .manufacturer = "esp32-IR",
        .sw_version = "1.0.0",
    };

    struct {
        const char *object_id;
        const char *name;
        const char *unit;
        const char *device_class;
        const char *value_template;
    } sensors[] = {
        {"power", "Power", "W", "power", "{{ value_json.power_w }}"},
        {"import", "Energy import", "kWh", "energy", "{{ value_json.import_kwh }}"},
        {"export", "Energy export", "kWh", "energy", "{{ value_json.export_kwh }}"},
    };

    char topic[MQTT_TOPIC_MAX_LEN];
    char json[640];
    for (size_t i = 0; i < sizeof(sensors) / sizeof(sensors[0]); ++i) {
        if (snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config",
                     device_id, sensors[i].object_id) >= (int)sizeof(topic)) {
            continue;
        }
        if (mqtt_build_discovery_json(&g_config, &device, sensors[i].object_id,
                                      sensors[i].name, sensors[i].unit,
                                      sensors[i].device_class,
                                      sensors[i].value_template, json,
                                      sizeof(json)) != ESP_OK) {
            continue;
        }
        int id = esp_mqtt_client_publish(g_client, topic, json, 0, 1, 1);
        if (id < 0) {
            ESP_LOGW(TAG, "Discovery publish failed for %s", sensors[i].object_id);
        }
    }
}

static void publish_state(void)
{
    char topic[MQTT_TOPIC_MAX_LEN];
    if (mqtt_build_topic(&g_config, "state", topic, sizeof(topic)) != ESP_OK) {
        return;
    }
    double power = 0.0;
    bool valid = false;
    double import_kwh = 0.0;
    double export_kwh = 0.0;
    portENTER_CRITICAL(&g_value_lock);
    power = g_power_w;
    valid = g_power_valid;
    import_kwh = g_import_kwh;
    export_kwh = g_export_kwh;
    portEXIT_CRITICAL(&g_value_lock);

    char json[192];
    if (mqtt_build_state_json(power, valid, import_kwh, export_kwh,
                              (int64_t)time(NULL), json, sizeof(json)) != ESP_OK) {
        return;
    }
    esp_mqtt_client_publish(g_client, topic, json, 0, 0, 0);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        g_connected = true;
        ESP_LOGI(TAG, "Connected to broker %s:%u", g_config.host,
                 (unsigned int)g_config.port);
        char status_topic[MQTT_TOPIC_MAX_LEN];
        if (mqtt_build_topic(&g_config, "status", status_topic,
                             sizeof(status_topic)) == ESP_OK) {
            esp_mqtt_client_publish(g_client, status_topic, "online", 0, 1, 1);
        }
        publish_discovery();
        publish_state();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        g_connected = false;
        ESP_LOGW(TAG, "Disconnected from broker");
        break;
    case MQTT_EVENT_ERROR:
        g_connected = false;
        ESP_LOGW(TAG, "MQTT error; will retry");
        break;
    default:
        break;
    }
}

static esp_err_t start_client(void)
{
    if (g_started || !g_config.enabled || !mqtt_config_is_valid(&g_config)) {
        return ESP_OK;
    }

    char uri[128];
    int written = snprintf(uri, sizeof(uri), "mqtt://%s:%u", g_config.host,
                           (unsigned int)g_config.port);
    if (written < 0 || (size_t)written >= sizeof(uri)) {
        ESP_LOGE(TAG, "MQTT broker URI too long");
        return ESP_ERR_INVALID_ARG;
    }

    char client_id[32];
    char device_id[16];
    device_id_string(device_id, sizeof(device_id));
    snprintf(client_id, sizeof(client_id), "esp32ir-%s", device_id);

    esp_mqtt_client_config_t config = {
        .broker.address.uri = uri,
        .credentials.client_id = client_id,
        .session.keepalive = 60,
        .session.last_will.topic = NULL,
    };
    if (g_config.username[0] != '\0') {
        config.credentials.username = g_config.username;
        config.credentials.authentication.password = g_config.password;
    }

    /* Last will: mark the device offline if it disappears without a clean
     * disconnect, so Home Assistant shows "unavailable" instead of stale data. */
    char will_topic[MQTT_TOPIC_MAX_LEN];
    if (mqtt_build_topic(&g_config, "status", will_topic, sizeof(will_topic)) ==
        ESP_OK) {
        config.session.last_will.topic = will_topic;
        config.session.last_will.msg = "offline";
        config.session.last_will.msg_len = 0;
        config.session.last_will.qos = 1;
        config.session.last_will.retain = 1;
    }

    g_client = esp_mqtt_client_init(&config);
    if (g_client == NULL) {
        ESP_LOGE(TAG, "Could not create MQTT client");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                   NULL);
    esp_err_t err = esp_mqtt_client_start(g_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(g_client);
        g_client = NULL;
        return err;
    }
    g_started = true;
    ESP_LOGI(TAG, "MQTT bridge started for %s", uri);
    return ESP_OK;
}

esp_err_t mqtt_publish_set_config(const mqtt_config_t *config)
{
    if (config == NULL || !mqtt_config_is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_config(config);
    if (err != ESP_OK) {
        return err;
    }
    g_config = *config;

    if (g_client != NULL) {
        esp_mqtt_client_stop(g_client);
        esp_mqtt_client_destroy(g_client);
        g_client = NULL;
        g_started = false;
        g_connected = false;
    }
    if (g_config.enabled) {
        return start_client();
    }
    return ESP_OK;
}

static void mqtt_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MQTT_TICK_MS));
        if (g_started && g_connected) {
            publish_state();
        }
    }
}

esp_err_t mqtt_publish_init(void)
{
    load_config();
    if (!mqtt_config_is_valid(&g_config)) {
        ESP_LOGW(TAG, "Stored MQTT configuration is invalid; bridge disabled");
        g_config.enabled = false;
    }
    esp_err_t err = start_client();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT bridge could not start; continuing without it");
    }
    if (xTaskCreate(mqtt_task, "mqtt", MQTT_TASK_STACK, NULL, MQTT_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start MQTT publish task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void mqtt_publish_power(double watts, bool valid)
{
    portENTER_CRITICAL(&g_value_lock);
    g_power_w = watts;
    g_power_valid = valid;
    portEXIT_CRITICAL(&g_value_lock);
}

void mqtt_publish_import(double kwh)
{
    portENTER_CRITICAL(&g_value_lock);
    g_import_kwh = kwh;
    portEXIT_CRITICAL(&g_value_lock);
}

void mqtt_publish_export(double kwh)
{
    portENTER_CRITICAL(&g_value_lock);
    g_export_kwh = kwh;
    portEXIT_CRITICAL(&g_value_lock);
}

bool mqtt_publish_connected(void)
{
    return g_connected;
}
