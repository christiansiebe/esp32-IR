#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sml_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SML_ADAPTER_NONE,
    SML_ADAPTER_CRC_VALID,
    SML_ADAPTER_CRC_INVALID,
} sml_adapter_result_t;

void sml_adapter_reset(void);
sml_adapter_result_t sml_adapter_feed_byte(uint8_t byte, uint64_t timestamp_us);

void sml_adapter_telegram_callback(uint64_t timestamp_us, bool crc_valid,
                                  const sml_transport_value_t *values,
                                  uint8_t value_count);

#ifdef __cplusplus
}
#endif
