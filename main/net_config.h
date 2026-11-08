#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Length ceilings (bytes, excluding the terminating NUL). */
#define NET_CONFIG_SSID_MAX 32
#define NET_CONFIG_WIFI_PASS_MAX 64
#define NET_CONFIG_WIFI_PASS_MIN 8
#define NET_CONFIG_MQTT_URI_MAX 160
#define NET_CONFIG_MQTT_PREFIX_MAX 80
#define NET_CONFIG_MQTT_USER_MAX 64
#define NET_CONFIG_MQTT_PASS_MAX 96

/* Wi-Fi SSID: 1..NET_CONFIG_SSID_MAX bytes, no control chars (0x00..0x1f, 0x7f).
 * On success writes nothing; on failure writes a short human-readable reason
 * into err (always NUL-terminated within err_size when err_size > 0). */
bool net_config_validate_ssid(const char *ssid, char *err, size_t err_size);

/* Wi-Fi password: empty string is allowed (open network / "keep stored"), else
 * NET_CONFIG_WIFI_PASS_MIN..NET_CONFIG_WIFI_PASS_MAX bytes, no control chars. */
bool net_config_validate_wifi_pass(const char *pass, char *err, size_t err_size);

/* MQTT broker URI: non-empty, scheme must be one of mqtt/mqtts/ws/wss followed
 * by "://", then a non-empty host (anything after "://" up to the first '/'),
 * no spaces or control chars anywhere, whole string < NET_CONFIG_MQTT_URI_MAX. */
bool net_config_validate_mqtt_uri(const char *uri, char *err, size_t err_size);

/* MQTT topic prefix: surrounding '/' are trimmed, the trimmed result must be
 * non-empty, contain no spaces/control chars, and be < NET_CONFIG_MQTT_PREFIX_MAX
 * (mirrors the rule in mqtt_topics.c prefix_valid). */
bool net_config_validate_mqtt_prefix(const char *prefix, char *err, size_t err_size);

/* Like net_config_validate_mqtt_prefix, but also copies the trimmed prefix into
 * out (NUL-terminated within out_size). Use this before persisting: the stored
 * value must already be the trimmed one, otherwise a prefix that only fits after
 * trimming would be rejected by the loader. out may be NULL. */
bool net_config_normalize_mqtt_prefix(const char *prefix, char *out, size_t out_size,
                                      char *err, size_t err_size);

/* Generic MQTT credential (user/pass): empty allowed, else length <= max_len,
 * no control chars. label is only used in the error text. */
bool net_config_validate_mqtt_credential(const char *value, size_t max_len,
                                         const char *label, char *err, size_t err_size);

#endif /* NET_CONFIG_H */
