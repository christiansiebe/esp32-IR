/* Host tests for the pure MQTT topic / HA discovery builders. */
#include <stdio.h>
#include <string.h>

#include "mqtt_topics.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

static void test_build(void)
{
    mqtt_topics_t t;
    CHECK(mqtt_topics_build("esp32ir/receiver", &t) == 0, "build ok");
    CHECK(strcmp(t.base, "esp32ir/receiver") == 0, "base kept");
    CHECK(strcmp(t.power, "esp32ir/receiver/power") == 0, "power topic");
    CHECK(strcmp(t.history, "esp32ir/receiver/history") == 0, "history topic");
    CHECK(strcmp(t.status, "esp32ir/receiver/status") == 0, "status topic");
    CHECK(strcmp(t.state, "esp32ir/receiver/state") == 0, "state topic");
    CHECK(strcmp(t.event_reboot, "esp32ir/receiver/event/reboot") == 0,
          "reboot event topic");

    /* Surrounding slashes are trimmed, so both spellings agree. */
    mqtt_topics_t t2;
    CHECK(mqtt_topics_build("/esp32ir/receiver/", &t2) == 0, "slashes ok");
    CHECK(strcmp(t2.power, t.power) == 0, "trimmed equals un-trimmed");

    CHECK(mqtt_topics_build("", &t) == -1, "empty prefix rejected");
    CHECK(mqtt_topics_build("///", &t) == -1, "slash-only prefix rejected");
    CHECK(mqtt_topics_build("has space", &t) == -1, "space rejected");
}

static void test_obis(void)
{
    char out[MQTT_TOPIC_MAX];
    CHECK(mqtt_topic_obis("esp32ir/receiver", 1, "1-0:16.7.0", out,
                          sizeof(out)) == 0,
          "obis topic ok");
    CHECK(strcmp(out, "esp32ir/receiver/sensor/1/obis/1-0:16.7.0*255/value") ==
              0,
          "obis topic spelling (SMLReader style)");

    CHECK(mqtt_topic_obis("esp32ir/receiver", 2, "1-0:1.8.0", out,
                          sizeof(out)) == 0,
          "obis topic second sensor");
    CHECK(strstr(out, "/sensor/2/") != NULL, "sensor index embedded");

    char small[16];
    CHECK(mqtt_topic_obis("esp32ir/receiver", 1, "1-0:16.7.0", small,
                          sizeof(small)) == -1,
          "obis topic overflow rejected");
}

static void test_discovery(void)
{
    char out[MQTT_TOPIC_MAX];
    CHECK(mqtt_discovery_topic("esp32ir/receiver", "sensor", "esp32ir_recv",
                               "power", out, sizeof(out)) == 0,
          "discovery topic ok");
    CHECK(strcmp(out,
                 "esp32ir/receiver/homeassistant/sensor/esp32ir_recv/power/"
                 "config") == 0,
          "discovery topic spelling");

    char payload[512];
    CHECK(mqtt_discovery_payload("Leistung", "esp32ir_recv_power",
                                 "esp32ir/receiver/power", "W", "power",
                                 "measurement", "esp32ir/receiver/status",
                                 payload, sizeof(payload)) == 0,
          "discovery payload ok");
    CHECK(strstr(payload, "\"name\":\"Leistung\"") != NULL, "payload name");
    CHECK(strstr(payload, "\"unit_of_measurement\":\"W\"") != NULL,
          "payload unit");
    CHECK(strstr(payload, "\"device_class\":\"power\"") != NULL,
          "payload device_class");
    CHECK(strstr(payload, "\"state_class\":\"measurement\"") != NULL,
          "payload state_class");
    CHECK(strstr(payload,
                 "\"availability_topic\":\"esp32ir/receiver/status\"") != NULL,
          "payload availability");
    CHECK(payload[0] == '{' && payload[strlen(payload) - 1] == '}',
          "payload is a JSON object");
    CHECK(strstr(payload, ",,") == NULL, "no empty JSON field");

    /* Optional fields omitted entirely. */
    char minimal[256];
    CHECK(mqtt_discovery_payload("Import", "esp32ir_recv_import",
                                 "esp32ir/receiver/sensor/1/obis/1-0:1.8.0*255/"
                                 "value",
                                 "kWh", NULL, "total_increasing", NULL, minimal,
                                 sizeof(minimal)) == 0,
          "minimal payload ok");
    CHECK(strstr(minimal, "device_class") == NULL, "omitted field absent");

    char small[8];
    CHECK(mqtt_discovery_payload("Leistung", "id", "topic", "W", "power",
                                 "measurement", "status", small,
                                 sizeof(small)) == -1,
          "payload overflow rejected");
    CHECK(small[0] == '\0', "overflow leaves buffer empty");
}

int main(void)
{
    test_build();
    test_obis();
    test_discovery();
    printf("mqtt topics: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
