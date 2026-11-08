/* Receiver-only MQTT bridge.
 *
 * Publishes live power, a finished-minute record and Home Assistant discovery
 * over esp-mqtt (IDF v6: managed component espressif/mqtt). The string layout
 * lives in mqtt_topics.c and is unit-tested on the host; this file is the thin
 * esp-mqtt glue. Configuration comes from NVS (namespace "mqttcfg"); without a
 * broker URI the bridge does not start, so the feature is opt-in.
 */
#pragma once

#include <stdbool.h>

#include "power_history.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the bridge if a broker URI is configured. Safe to call when
 * unconfigured (it then does nothing). */
void mqtt_bridge_start(void);

/* Publish the current live power (W) to <base>/power, retained. Cheap no-op
 * when the bridge is not connected. */
void mqtt_bridge_publish_power(double watts);

/* Publish a cumulative meter reading to <base>/import or <base>/export (kWh,
 * retained). Home Assistant's total_increasing energy sensors record the
 * statistics history from these, so HA mirrors the meter without polling. The
 * value is the raw SML reading (value * 10^scaler, i.e. Wh) and is converted to
 * kWh here. Publish each counter only when its own reading arrives, so a
 * not-yet-seen counter is never sent as a spurious 0 (which a total_increasing
 * sensor would read as a meter reset). Cheap no-op when not connected. */
void mqtt_bridge_publish_import(double raw, int8_t scaler);
void mqtt_bridge_publish_export(double raw, int8_t scaler);

/* Publish a finished minute record to <base>/history as JSON. */
void mqtt_bridge_publish_history(const power_history_record_t *record);

/* True once a broker URI was loaded and the client was started. */
bool mqtt_bridge_started(void);

/* Load the MQTT settings from NVS (namespace "mqttcfg", keys prefix/uri/user/
 * pass). Exposed for diagnostics; mqtt_bridge_start() calls it internally. */
bool mqtt_bridge_load_config(void);

/* Persist one MQTT setting and restart the bridge so the change takes effect.
 * key is one of "mqtt-uri", "mqtt-prefix", "mqtt-user" or "mqtt-pass"; value is
 * validated first (net_config.c) and written to NVS only when valid. On success
 * the running client is stopped, destroyed and re-created from the new NVS
 * values. On failure the stored value and the running client are left untouched
 * and a short reason is written into err. Empty user/pass clear the credential. */
bool mqtt_bridge_set_config(const char *key, const char *value, char *err,
                            size_t err_size);

/* One-line human-readable status for the web console. */
void mqtt_bridge_status(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
