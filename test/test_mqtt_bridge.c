/* Host tests for the MQTT bridge payload builders and the Home Assistant
 * discovery entities. No esp-mqtt, no ESP-IDF: the state strings and the
 * discovery payloads are the part that breaks silently (a JSON state without a
 * value_template reads as "unknown" in HA; a wrong unit breaks statistics). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_publish.h"
#include "mqtt_topics.h"

static int g_checks;
static int g_failures;

static int near(double a, double b)
{
    return fabs(a - b) < 1e-6;
}

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

static void test_energy_to_kwh(void)
{
    /* Wh with scaler -1: e.g. raw 123456, 10^-1 = 12345.6 Wh = 12.3456 kWh. */
    CHECK(near(mqtt_energy_to_kwh(123456.0, -1), 12.3456), "Wh scaler -1 to kWh");
    /* Wh with scaler 0: 29930 Wh = 29.930 kWh. */
    CHECK(near(mqtt_energy_to_kwh(29930.0, 0), 29.93), "Wh scaler 0 to kWh");
    /* scaler 1: 1000 * 10^1 = 10000 Wh = 10 kWh. */
    CHECK(near(mqtt_energy_to_kwh(1000.0, 1), 10.0), "Wh scaler 1 to kWh");
    /* Non-finite input is clamped to 0, never NaN into the payload. */
    CHECK(near(mqtt_energy_to_kwh(1.0 / 0.0, 0), 0.0), "infinity clamped to 0");
}

static void test_power_payload(void)
{
    char out[32];
    CHECK(mqtt_power_payload(1234.5, out, sizeof(out)) == 0, "power built");
    CHECK(strcmp(out, "1234.5") == 0, "power is a bare number");
    CHECK(mqtt_power_payload(-87.25, out, sizeof(out)) == 0, "negative power built");
    CHECK(strcmp(out, "-87.2") == 0, "negative power rounded to one decimal");
    CHECK(mqtt_power_payload(0.0, out, sizeof(out)) == 0, "zero power built");
    CHECK(strcmp(out, "0.0") == 0, "zero power");
    /* Not JSON: a Home Assistant state_topic without value_template needs a
     * bare number, so the payload must not start with '{'. */
    CHECK(out[0] != '{', "power payload is not JSON");

    char small[3];
    CHECK(mqtt_power_payload(1234.5, small, sizeof(small)) == -1, "overflow rejected");
    CHECK(small[0] == '\0', "overflow leaves buffer empty");
}

static void test_energy_payload(void)
{
    char out[32];
    CHECK(mqtt_energy_payload(42.5, out, sizeof(out)) == 0, "energy built");
    CHECK(strcmp(out, "42.500") == 0, "energy is a bare number with 3 decimals");
    CHECK(mqtt_energy_payload(0.0, out, sizeof(out)) == 0, "zero energy built");
    CHECK(strcmp(out, "0.000") == 0, "zero energy");

    char small[3];
    CHECK(mqtt_energy_payload(42.5, small, sizeof(small)) == -1, "overflow rejected");
}

static void test_discovery_entities(void)
{
    /* The three entities the bridge announces. Their topics and classes must be
     * exactly what Home Assistant expects for live power + the two cumulative
     * energy counters (total_increasing so HA's statistics engine records the
     * history itself). */
    mqtt_topics_t t;
    CHECK(mqtt_topics_build("esp32ir/receiver", &t) == 0, "topics built");
    CHECK(strcmp(t.power, "esp32ir/receiver/power") == 0, "power state topic");

    char import_topic[MQTT_TOPIC_MAX];
    char export_topic[MQTT_TOPIC_MAX];
    snprintf(import_topic, sizeof(import_topic), "%s/import", t.base);
    snprintf(export_topic, sizeof(export_topic), "%s/export", t.base);
    CHECK(strcmp(import_topic, "esp32ir/receiver/import") == 0, "import topic");
    CHECK(strcmp(export_topic, "esp32ir/receiver/export") == 0, "export topic");

    char topic[MQTT_TOPIC_MAX];
    char payload[512];

    CHECK(mqtt_discovery_topic("esp32ir/receiver", "sensor", "esp32ir_recv",
                               "power", topic, sizeof(topic)) == 0,
          "power discovery topic");
    CHECK(mqtt_discovery_payload("Leistung", "esp32ir_recv_power", t.power, "W",
                                 "power", "measurement", t.status, payload,
                                 sizeof(payload)) == 0,
          "power discovery payload");
    CHECK(strstr(payload, "\"device_class\":\"power\"") != NULL, "power class");
    CHECK(strstr(payload, "\"state_class\":\"measurement\"") != NULL,
          "power state_class");
    CHECK(strstr(payload, "\"state_topic\":\"esp32ir/receiver/power\"") != NULL,
          "power state_topic");

    CHECK(mqtt_discovery_payload("Bezug gesamt", "esp32ir_recv_import",
                                 import_topic, "kWh", "energy",
                                 "total_increasing", t.status, payload,
                                 sizeof(payload)) == 0,
          "import discovery payload");
    CHECK(strstr(payload, "\"device_class\":\"energy\"") != NULL, "import class");
    CHECK(strstr(payload, "\"state_class\":\"total_increasing\"") != NULL,
          "import state_class total_increasing");
    CHECK(strstr(payload, "\"unit_of_measurement\":\"kWh\"") != NULL, "import unit");
    CHECK(strstr(payload, "\"state_topic\":\"esp32ir/receiver/import\"") != NULL,
          "import state_topic");

    CHECK(mqtt_discovery_payload("Einspeisung gesamt", "esp32ir_recv_export",
                                 export_topic, "kWh", "energy",
                                 "total_increasing", t.status, payload,
                                 sizeof(payload)) == 0,
          "export discovery payload");
    CHECK(strstr(payload, "\"state_topic\":\"esp32ir/receiver/export\"") != NULL,
          "export state_topic");
    CHECK(strstr(payload, "\"device_class\":\"energy\"") != NULL, "export class");
    CHECK(strstr(payload, "\"unit_of_measurement\":\"kWh\"") != NULL, "export unit");
}

int main(void)
{
    test_energy_to_kwh();
    test_power_payload();
    test_energy_payload();
    test_discovery_entities();

    printf("mqtt bridge: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
