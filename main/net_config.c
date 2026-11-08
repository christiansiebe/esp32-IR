/* Validation of the WLAN/MQTT configuration values entered through the web
 * console. Pure C (no IDF), so it is host-tested in test_net_config.c; the
 * callers (main.c / mqtt_bridge.c) reject a bad value before it reaches NVS or
 * the driver. */
#include "net_config.h"

#include <stdio.h>
#include <string.h>

/* Write a short reason into the caller's buffer. err may be NULL or err_size 0,
 * in which case nothing is written; otherwise the result is always
 * NUL-terminated (snprintf truncates rather than overflowing). */
static void set_err(char *err, size_t err_size, const char *msg)
{
    if (err == NULL || err_size == 0) {
        return;
    }
    snprintf(err, err_size, "%s", msg);
}

/* Same, with the credential label prefixed to the reason. */
static void set_err_label(char *err, size_t err_size, const char *label,
                          const char *reason)
{
    if (err == NULL || err_size == 0) {
        return;
    }
    snprintf(err, err_size, "%s %s", label != NULL ? label : "credential", reason);
}

/* The C0 controls plus DEL; never allowed in a config value. */
static bool is_control(unsigned char c)
{
    return c <= 0x1f || c == 0x7f;
}

bool net_config_validate_ssid(const char *ssid, char *err, size_t err_size)
{
    if (ssid == NULL) {
        set_err(err, err_size, "ssid missing");
        return false;
    }
    size_t len = strlen(ssid);
    if (len == 0) {
        set_err(err, err_size, "ssid empty");
        return false;
    }
    if (len > NET_CONFIG_SSID_MAX) {
        set_err(err, err_size, "ssid too long");
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (is_control((unsigned char)ssid[i])) {
            set_err(err, err_size, "ssid has control char");
            return false;
        }
    }
    return true;
}

bool net_config_validate_wifi_pass(const char *pass, char *err, size_t err_size)
{
    if (pass == NULL) {
        set_err(err, err_size, "wifi pass missing");
        return false;
    }
    size_t len = strlen(pass);
    /* Empty means an open network or "keep the stored key". */
    if (len == 0) {
        return true;
    }
    if (len < NET_CONFIG_WIFI_PASS_MIN) {
        set_err(err, err_size, "wifi pass too short");
        return false;
    }
    if (len > NET_CONFIG_WIFI_PASS_MAX) {
        set_err(err, err_size, "wifi pass too long");
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (is_control((unsigned char)pass[i])) {
            set_err(err, err_size, "wifi pass has control char");
            return false;
        }
    }
    return true;
}

bool net_config_validate_mqtt_uri(const char *uri, char *err, size_t err_size)
{
    if (uri == NULL) {
        set_err(err, err_size, "uri missing");
        return false;
    }
    size_t len = strlen(uri);
    if (len == 0) {
        set_err(err, err_size, "uri empty");
        return false;
    }
    if (len >= NET_CONFIG_MQTT_URI_MAX) {
        set_err(err, err_size, "uri too long");
        return false;
    }
    /* Spaces are 0x20 and are lumped in with the controls: a broker URI must
     * not contain either. */
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)uri[i];
        if (c <= 0x20 || c == 0x7f) {
            set_err(err, err_size, "uri has space/control char");
            return false;
        }
    }

    static const char *const schemes[] = {"mqtt", "mqtts", "ws", "wss"};
    const char *host = NULL;
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); ++i) {
        size_t sl = strlen(schemes[i]);
        if (strncmp(uri, schemes[i], sl) == 0 &&
            strncmp(uri + sl, "://", 3) == 0) {
            host = uri + sl + 3;
            break;
        }
    }
    if (host == NULL) {
        set_err(err, err_size, "uri scheme must be mqtt/mqtts/ws/wss");
        return false;
    }
    /* Host is everything after "://" up to the first '/'; it must be non-empty,
     * so "mqtt://" and "mqtt:///path" are rejected. */
    if (*host == '\0' || *host == '/') {
        set_err(err, err_size, "uri host missing");
        return false;
    }
    return true;
}

bool net_config_validate_mqtt_prefix(const char *prefix, char *err, size_t err_size)
{
    if (prefix == NULL) {
        set_err(err, err_size, "prefix missing");
        return false;
    }
    while (*prefix == '/') {
        ++prefix;
    }
    size_t len = strlen(prefix);
    while (len > 0 && prefix[len - 1] == '/') {
        --len;
    }
    if (len == 0) {
        set_err(err, err_size, "prefix empty");
        return false;
    }
    if (len >= NET_CONFIG_MQTT_PREFIX_MAX) {
        set_err(err, err_size, "prefix too long");
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)prefix[i];
        if (c <= 0x20 || c == 0x7f) {
            set_err(err, err_size, "prefix has space/control char");
            return false;
        }
    }
    return true;
}

bool net_config_validate_mqtt_credential(const char *value, size_t max_len,
                                         const char *label, char *err, size_t err_size)
{
    if (value == NULL) {
        set_err_label(err, err_size, label, "missing");
        return false;
    }
    size_t len = strlen(value);
    if (len == 0) {
        return true;
    }
    if (len > max_len) {
        set_err_label(err, err_size, label, "too long");
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (is_control((unsigned char)value[i])) {
            set_err_label(err, err_size, label, "has control char");
            return false;
        }
    }
    return true;
}
