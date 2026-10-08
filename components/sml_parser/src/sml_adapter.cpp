#include <stdint.h>
#include <string.h>

#include "sml.h"
#include "sml_adapter.h"

static const unsigned char OBIS_IMPORT_ENERGY[6] = {0x01, 0x00, 0x01, 0x08, 0x00, 0xff};
static const unsigned char OBIS_EXPORT_ENERGY[6] = {0x01, 0x00, 0x02, 0x08, 0x00, 0xff};
static const unsigned char OBIS_ACTIVE_POWER[6] = {0x01, 0x00, 0x10, 0x07, 0x00, 0xff};
static sml_transport_value_t pending_values[SML_TRANSPORT_MAX_VALUES];
static uint8_t pending_value_count;
static uint64_t pending_timestamp_us;

extern "C" void sml_adapter_reset(void)
{
    smlReset();
    pending_value_count = 0;
    pending_timestamp_us = 0;
}

static void remember_value(const unsigned char *obis, sml_units_t unit,
                           uint64_t timestamp_us)
{
    long long value;
    signed char scaler = 0;
    if (!smlOBISByUnitChecked(value, scaler, unit)) {
        return;
    }
    uint8_t index = 0;
    while (index < pending_value_count &&
           memcmp(pending_values[index].obis, obis, 6) != 0) {
        ++index;
    }
    if (index == pending_value_count) {
        if (pending_value_count >= SML_TRANSPORT_MAX_VALUES) {
            return;
        }
        ++pending_value_count;
    }

    memcpy(pending_values[index].obis, obis, 6);
    pending_values[index].value = value;
    pending_values[index].scaler = scaler;
    pending_values[index].unit = static_cast<uint8_t>(unit);
    pending_timestamp_us = timestamp_us;
}

extern "C" sml_adapter_result_t sml_adapter_feed_byte(uint8_t byte,
                                                        uint64_t timestamp_us)
{
    unsigned char parser_byte = byte;
    sml_states_t state = smlState(parser_byte);

    if (state == SML_LISTEND) {
        if (smlOBISCheck(OBIS_IMPORT_ENERGY)) {
            remember_value(OBIS_IMPORT_ENERGY, SML_WATT_HOUR, timestamp_us);
        } else if (smlOBISCheck(OBIS_EXPORT_ENERGY)) {
            remember_value(OBIS_EXPORT_ENERGY, SML_WATT_HOUR, timestamp_us);
        } else if (smlOBISCheck(OBIS_ACTIVE_POWER)) {
            remember_value(OBIS_ACTIVE_POWER, SML_WATT, timestamp_us);
        }
    }

    if (state == SML_FINAL) {
        sml_adapter_telegram_callback(pending_value_count != 0
                                          ? pending_timestamp_us
                                          : timestamp_us,
                                      true,
                                      pending_values, pending_value_count);
        pending_value_count = 0;
        return SML_ADAPTER_CRC_VALID;
    }
    if (state == SML_CHECKSUM_ERROR) {
        sml_adapter_telegram_callback(timestamp_us, false, nullptr, 0);
        pending_value_count = 0;
        pending_timestamp_us = 0;
        return SML_ADAPTER_CRC_INVALID;
    }
    if (state == SML_UNEXPECTED) {
        pending_value_count = 0;
        pending_timestamp_us = 0;
    }
    return SML_ADAPTER_NONE;
}
