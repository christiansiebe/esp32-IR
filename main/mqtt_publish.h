#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Pure payload builders for the MQTT bridge.
 *
 * Deliberately free of esp-mqtt and ESP-IDF (like mqtt_topics.c): the wire
 * strings are the part that breaks silently in Home Assistant (a JSON state
 * without a value_template is not a number, so the entity reads "unknown").
 * Keeping the formatting and the energy unit conversion here lets the host
 * tests cover them; the esp-mqtt glue in mqtt_bridge.c stays a thin caller.
 *
 * The three state topics carry a bare number so a Home Assistant sensor needs
 * no value_template:
 *   <base>/power   live power in W,        device_class power
 *   <base>/import  metered import in kWh,  device_class energy, total_increasing
 *   <base>/export  metered export in kWh,  device_class energy, total_increasing
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Convert a raw SML energy reading (value * 10^scaler) to kWh. The meters
 * report Wh, so the result is divided by 1000. A non-finite input yields 0.0. */
double mqtt_energy_to_kwh(double raw_value, int8_t scaler);

/* Format live power as the bare-number state string ("1234.5"). Returns 0 on
 * success, -1 on overflow (out is left NUL-terminated). */
int mqtt_power_payload(double watts, char *out, size_t out_size);

/* Format an energy reading in kWh as the bare-number state string ("42.500").
 * Returns 0 on success, -1 on overflow. */
int mqtt_energy_payload(double kwh, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
