#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Last ESP-NOW channel the sender actually worked on.
 *
 * The sender has no channel of its own: the receiver's SoftAP is the channel
 * lighthouse, and the sender learns the channel from a scan of that AP. The
 * learned value lived only in a RAM static, so every boot started from
 * CONFIG_ESPNOW_CHANNEL and re-ran the discovery:
 *
 *   - the boot scan waits up to 30 s in a 1 s loop for the AP to appear, and
 *   - if the AP is not up in time the sender pins the compile-time channel and
 *     only heals once the link watchdog notices the silence (60 s + grace).
 *
 * Both are needless when the channel is remembered. The record persists the
 * last good channel so the next boot starts on it immediately; the periodic
 * re-align still corrects a router channel change afterwards.
 *
 * The record format and its validation are pure C so they can be host-tested
 * without NVS. The NVS binding lives in espnow_channel_store.c.
 */

#define ESPNOW_CHANNEL_MAGIC   0x4E484345u /* "ECHN" */
#define ESPNOW_CHANNEL_VERSION 1u

/* 2.4 GHz channels 1..13 are the only ones the ESP32 radio offers. Channel 0 is
 * the "not set" sentinel used throughout the sender code, so it is rejected. */
#define ESPNOW_CHANNEL_MIN 1
#define ESPNOW_CHANNEL_MAX 13

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint8_t  channel;
    uint8_t  reserved2[3];
    uint32_t checksum;
} espnow_channel_record_t;

/* True when `channel` is a real 2.4 GHz channel (1..13). */
bool espnow_channel_is_valid(uint8_t channel);

/* Fill a record for the given channel (reserved zeroed, checksum computed). */
void espnow_channel_make_record(espnow_channel_record_t *record, uint8_t channel);

/* magic + version + valid channel + checksum. */
bool espnow_channel_record_is_valid(const espnow_channel_record_t *record);

/* NVS-backed persistence (implemented in espnow_channel_store.c). `save`
 * rejects an out-of-range channel so a bad scan can never poison the store;
 * `load` returns ESP_OK with *found=false when nothing (valid) is stored. */
esp_err_t espnow_channel_store_save(uint8_t channel);
esp_err_t espnow_channel_store_load(uint8_t *channel, bool *found);
