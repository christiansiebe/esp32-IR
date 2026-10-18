#include "mqtt_publish.h"

#include <math.h>
#include <stdio.h>

double mqtt_energy_to_kwh(double raw_value, int8_t scaler)
{
    if (!isfinite(raw_value)) {
        return 0.0;
    }
    double scaled = raw_value;
    for (int exponent = 0; exponent < scaler; ++exponent) {
        scaled *= 10.0;
    }
    for (int exponent = 0; exponent > scaler; --exponent) {
        scaled /= 10.0;
    }
    if (!isfinite(scaled)) {
        return 0.0;
    }
    /* The meter reports Wh; Home Assistant energy sensors use kWh. */
    return scaled / 1000.0;
}

int mqtt_power_payload(double watts, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return -1;
    }
    if (!isfinite(watts)) {
        watts = 0.0;
    }
    int written = snprintf(out, out_size, "%.1f", watts);
    if (written < 0 || (size_t)written >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return 0;
}

int mqtt_energy_payload(double kwh, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return -1;
    }
    if (!isfinite(kwh)) {
        kwh = 0.0;
    }
    int written = snprintf(out, out_size, "%.3f", kwh);
    if (written < 0 || (size_t)written >= out_size) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return 0;
}
