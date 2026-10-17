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

/* Publish a finished minute record to <base>/history as JSON. */
void mqtt_bridge_publish_history(const power_history_record_t *record);

/* True once a broker URI was loaded and the client was started. */
bool mqtt_bridge_started(void);

/* Load the MQTT settings from NVS (namespace "mqttcfg", keys prefix/uri/user/
 * pass). Exposed for diagnostics; mqtt_bridge_start() calls it internally. */
bool mqtt_bridge_load_config(void);

/* One-line human-readable status for the web console. */
void mqtt_bridge_status(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
