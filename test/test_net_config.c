/* Host test for the WLAN/MQTT value validation (main/net_config.c).
 *
 * The web console writes these values into NVS and hands them to the Wi-Fi and
 * MQTT drivers, so a bad value must be rejected before it gets there. Pure C,
 * no IDF.
 */
#include <stdio.h>
#include <string.h>

#include "net_config.h"

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

/* A helper that fills a string with 'n' copies of 'c' and NUL-terminates. */
static void fill(char *buf, size_t n, char c)
{
    for (size_t i = 0; i < n; ++i) {
        buf[i] = c;
    }
    buf[n] = '\0';
}

int main(void)
{
    char err[128];

    /* --- SSID ---------------------------------------------------------- */
    char ssid32[NET_CONFIG_SSID_MAX + 1];
    fill(ssid32, NET_CONFIG_SSID_MAX, 'a');
    CHECK(net_config_validate_ssid("net", err, sizeof(err)), "ssid 3 bytes ok");
    CHECK(net_config_validate_ssid(ssid32, err, sizeof(err)),
          "ssid exactly 32 bytes ok");
    char ssid33[NET_CONFIG_SSID_MAX + 2];
    fill(ssid33, NET_CONFIG_SSID_MAX + 1, 'a');
    CHECK(!net_config_validate_ssid(ssid33, err, sizeof(err)),
          "ssid 33 bytes rejected");
    CHECK(strlen(err) > 0, "ssid 33 bytes sets an error text");
    CHECK(!net_config_validate_ssid("", err, sizeof(err)), "empty ssid rejected");
    CHECK(!net_config_validate_ssid(NULL, err, sizeof(err)), "NULL ssid rejected");
    CHECK(!net_config_validate_ssid("bad\tssid", err, sizeof(err)),
          "ssid with a tab rejected");
    CHECK(!net_config_validate_ssid("bad\x7fssid", err, sizeof(err)),
          "ssid with DEL rejected");

    /* err may be NULL or size 0: no crash, just the boolean result. */
    CHECK(net_config_validate_ssid("ok", NULL, 0), "NULL err is tolerated");
    CHECK(!net_config_validate_ssid(NULL, NULL, 0), "NULL err + NULL ssid");

    /* --- Wi-Fi password ------------------------------------------------ */
    char pass8[NET_CONFIG_WIFI_PASS_MIN + 1];
    fill(pass8, NET_CONFIG_WIFI_PASS_MIN, 'x');
    char pass7[NET_CONFIG_WIFI_PASS_MIN];
    fill(pass7, NET_CONFIG_WIFI_PASS_MIN - 1, 'x');
    char pass64[NET_CONFIG_WIFI_PASS_MAX + 1];
    fill(pass64, NET_CONFIG_WIFI_PASS_MAX, 'x');
    char pass65[NET_CONFIG_WIFI_PASS_MAX + 2];
    fill(pass65, NET_CONFIG_WIFI_PASS_MAX + 1, 'x');

    CHECK(net_config_validate_wifi_pass("", err, sizeof(err)),
          "empty wifi pass ok (open/keep)");
    CHECK(!net_config_validate_wifi_pass(pass7, err, sizeof(err)),
          "7-byte wifi pass rejected");
    CHECK(net_config_validate_wifi_pass(pass8, err, sizeof(err)),
          "8-byte wifi pass ok");
    CHECK(net_config_validate_wifi_pass(pass64, err, sizeof(err)),
          "64-byte wifi pass ok");
    CHECK(!net_config_validate_wifi_pass(pass65, err, sizeof(err)),
          "65-byte wifi pass rejected");
    CHECK(!net_config_validate_wifi_pass(NULL, err, sizeof(err)),
          "NULL wifi pass rejected");
    CHECK(!net_config_validate_wifi_pass("secret\nkey", err, sizeof(err)),
          "wifi pass with a control char rejected");

    /* --- MQTT URI ------------------------------------------------------ */
    CHECK(net_config_validate_mqtt_uri("mqtt://host", err, sizeof(err)),
          "mqtt://host ok");
    CHECK(net_config_validate_mqtt_uri("mqtts://h:8883", err, sizeof(err)),
          "mqtts://h:8883 ok");
    CHECK(net_config_validate_mqtt_uri("ws://h", err, sizeof(err)), "ws://h ok");
    CHECK(net_config_validate_mqtt_uri("wss://h", err, sizeof(err)), "wss://h ok");
    CHECK(net_config_validate_mqtt_uri("mqtt://h/path", err, sizeof(err)),
          "mqtt://h/path ok");
    CHECK(!net_config_validate_mqtt_uri("ftp://h", err, sizeof(err)),
          "ftp://h rejected");
    CHECK(!net_config_validate_mqtt_uri("mqtt://", err, sizeof(err)),
          "mqtt:// without host rejected");
    CHECK(!net_config_validate_mqtt_uri("host", err, sizeof(err)),
          "scheme-less host rejected");
    CHECK(!net_config_validate_mqtt_uri("mqtt://ho st", err, sizeof(err)),
          "uri with a space rejected");
    CHECK(!net_config_validate_mqtt_uri("mqtt://:1883", err, sizeof(err)),
          "port-only host rejected");
    CHECK(net_config_validate_mqtt_uri("mqtt://h:1883", err, sizeof(err)),
          "host with a port ok");
    CHECK(!net_config_validate_mqtt_uri("", err, sizeof(err)), "empty uri rejected");
    CHECK(!net_config_validate_mqtt_uri(NULL, err, sizeof(err)), "NULL uri rejected");
    CHECK(!net_config_validate_mqtt_uri("mqtt://h\t", err, sizeof(err)),
          "uri with a control char rejected");
    /* Whole string must stay strictly below the ceiling. */
    char uri159[NET_CONFIG_MQTT_URI_MAX];
    memset(uri159, 'a', sizeof(uri159));
    memcpy(uri159, "mqtt://", 7);
    uri159[sizeof(uri159) - 1] = '\0';
    CHECK(net_config_validate_mqtt_uri(uri159, err, sizeof(err)),
          "uri of length 159 ok");
    char uri160[NET_CONFIG_MQTT_URI_MAX + 1];
    memset(uri160, 'a', sizeof(uri160));
    memcpy(uri160, "mqtt://", 7);
    uri160[sizeof(uri160) - 1] = '\0';
    CHECK(!net_config_validate_mqtt_uri(uri160, err, sizeof(err)),
          "uri of length 160 rejected");

    /* --- MQTT prefix --------------------------------------------------- */
    CHECK(net_config_validate_mqtt_prefix("a/b", err, sizeof(err)), "prefix a/b ok");
    CHECK(net_config_validate_mqtt_prefix("/a/b/", err, sizeof(err)),
          "prefix /a/b/ ok (trimmed)");
    CHECK(!net_config_validate_mqtt_prefix("", err, sizeof(err)),
          "empty prefix rejected");
    CHECK(!net_config_validate_mqtt_prefix("/", err, sizeof(err)),
          "prefix '/' trims to empty and is rejected");
    CHECK(!net_config_validate_mqtt_prefix(" ", err, sizeof(err)),
          "prefix ' ' rejected");
    CHECK(!net_config_validate_mqtt_prefix(NULL, err, sizeof(err)),
          "NULL prefix rejected");
    CHECK(!net_config_validate_mqtt_prefix("a b", err, sizeof(err)),
          "prefix with a space rejected");
    char prefix79[NET_CONFIG_MQTT_PREFIX_MAX];
    fill(prefix79, NET_CONFIG_MQTT_PREFIX_MAX - 1, 'a');
    CHECK(net_config_validate_mqtt_prefix(prefix79, err, sizeof(err)),
          "prefix of length 79 ok");
    char prefix80[NET_CONFIG_MQTT_PREFIX_MAX + 1];
    fill(prefix80, NET_CONFIG_MQTT_PREFIX_MAX, 'a');
    CHECK(!net_config_validate_mqtt_prefix(prefix80, err, sizeof(err)),
          "prefix of length 80 rejected");

    /* --- MQTT prefix normalization ------------------------------------- */
    /* Surrounding '/' are stripped before the value is persisted, so a prefix
     * that only fits after trimming is stored in its trimmed form (the loader
     * would otherwise reject the raw value on the next boot). */
    char norm[NET_CONFIG_MQTT_PREFIX_MAX] = {0};
    CHECK(net_config_normalize_mqtt_prefix("/a/b/", norm, sizeof(norm), err,
                                           sizeof(err)) &&
              strcmp(norm, "a/b") == 0,
          "normalize trims surrounding slashes");
    CHECK(!net_config_normalize_mqtt_prefix("/", norm, sizeof(norm), err,
                                            sizeof(err)),
          "normalize rejects an all-slash prefix");
    CHECK(!net_config_normalize_mqtt_prefix("a b", norm, sizeof(norm), err,
                                            sizeof(err)),
          "normalize rejects an embedded space");
    /* 40 leading '/' + 40 chars: raw length 80 but the trimmed 40 are valid, so
     * normalize accepts and returns the 40-char form. */
    char raw80[82];
    memset(raw80, '/', 40);
    memset(raw80 + 40, 'b', 40);
    raw80[80] = '\0';
    CHECK(net_config_normalize_mqtt_prefix(raw80, norm, sizeof(norm), err,
                                           sizeof(err)) &&
              strcmp(norm, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0,
          "normalize keeps a trimmed 40-char prefix that was 80 raw");

    /* --- MQTT credential ----------------------------------------------- */
    char cred64[NET_CONFIG_MQTT_USER_MAX + 1];
    fill(cred64, NET_CONFIG_MQTT_USER_MAX, 'u');
    char cred65[NET_CONFIG_MQTT_USER_MAX + 2];
    fill(cred65, NET_CONFIG_MQTT_USER_MAX + 1, 'u');

    CHECK(net_config_validate_mqtt_credential("", NET_CONFIG_MQTT_USER_MAX,
                                              "user", err, sizeof(err)),
          "empty credential ok");
    CHECK(net_config_validate_mqtt_credential(cred64, NET_CONFIG_MQTT_USER_MAX,
                                              "user", err, sizeof(err)),
          "64-byte credential ok (max 64)");
    CHECK(!net_config_validate_mqtt_credential(cred65, NET_CONFIG_MQTT_USER_MAX,
                                               "user", err, sizeof(err)),
          "65-byte credential rejected (max 64)");
    CHECK(!net_config_validate_mqtt_credential("bad\npass", NET_CONFIG_MQTT_PASS_MAX,
                                               "pass", err, sizeof(err)),
          "credential with a control char rejected");
    CHECK(!net_config_validate_mqtt_credential(NULL, NET_CONFIG_MQTT_USER_MAX,
                                               "user", err, sizeof(err)),
          "NULL credential rejected");
    /* The label shows up in the error text. */
    CHECK(net_config_validate_mqtt_credential("bad\x7f", 96, "password", err,
                                              sizeof(err)) == false &&
              strstr(err, "password") != NULL,
          "the credential error names the label");
    /* A 96-byte password is fine for the pass ceiling. */
    char cred96[NET_CONFIG_MQTT_PASS_MAX + 1];
    fill(cred96, NET_CONFIG_MQTT_PASS_MAX, 'p');
    CHECK(net_config_validate_mqtt_credential(cred96, NET_CONFIG_MQTT_PASS_MAX,
                                              "pass", err, sizeof(err)),
          "96-byte credential ok (max 96)");

    printf("net config: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
