#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "mqtt_bridge.h"

/*
 * Optional MQTT bridge for the receiver.
 *
 * Publishes the latest meter values and registers the device with Home
 * Assistant via MQTT discovery. The connection is best effort: when no broker
 * is configured (or the connection fails) the device keeps running and the
 * dashboard is unaffected. Configuration is persisted in NVS.
 */

/* Load the saved configuration and start the client task. Safe to call once
 * from app_main. Never blocks on the network. */
esp_err_t mqtt_publish_init(void);

/* Current configuration (a copy). */
void mqtt_publish_get_config(mqtt_config_t *config);

/* Persist and apply a new configuration. Passing enabled=false disables the
 * bridge without clearing the host/user/password. */
esp_err_t mqtt_publish_set_config(const mqtt_config_t *config);

/* Store the latest meter values; published on the next MQTT tick. */
void mqtt_publish_power(double watts, bool valid);
void mqtt_publish_energy(double import_kwh, double export_kwh);

/* Connection state for the status/console output. */
bool mqtt_publish_connected(void);
