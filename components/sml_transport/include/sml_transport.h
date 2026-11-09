#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SML_TRANSPORT_MAGIC 0x534D4C31u
#define SML_TRANSPORT_VERSION 1u
#define SML_TRANSPORT_MAX_VALUES 3u

typedef struct __attribute__((packed)) {
    uint8_t obis[6];
    int64_t value;
    int8_t scaler;
    uint8_t unit;
} sml_transport_value_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t crc_valid;
    uint32_t sequence;
    uint64_t timestamp_us;
    uint8_t value_count;
} sml_transport_header_t;

typedef struct __attribute__((packed)) {
    sml_transport_header_t header;
    sml_transport_value_t values[SML_TRANSPORT_MAX_VALUES];
} sml_transport_packet_t;

/* Application-level acknowledgement of a received SML telegram.
 *
 * ESP-NOW's own send callback only proves the frame left the *sender's* radio
 * (and for the broadcast data path there is no per-frame ACK at all), so a
 * telegram lost over the air is only noticed later as a sequence gap - too late
 * to retry it. The receiver therefore answers every well-formed telegram with
 * this unicast ACK, and the sender stops retransmitting a telegram whose
 * sequence it has seen acknowledged. This makes delivery confirmed rather than
 * best-effort: an un-acked telegram is repeated until the receiver confirms it.
 *
 * `sequence` echoes the sml_transport_header_t.sequence being acknowledged. The
 * ACK is sent for any well-formed telegram (even a duplicate or one with a bad
 * upstream CRC): both arrived, so a retransmit would not help and would only
 * waste airtime. */
#define SML_TRANSPORT_ACK_MAGIC 0x534D4131u /* "SMA1" */
#define SML_TRANSPORT_ACK_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    uint32_t sequence; /* the telegram sequence being acknowledged */
} sml_transport_ack_t;

/* Fill `out` with the ACK for `sequence`. Header-only so the sender, the
 * receiver and the host tests all build/parse it identically. */
static inline void sml_transport_ack_build(sml_transport_ack_t *out,
                                           uint32_t sequence)
{
    out->magic = SML_TRANSPORT_ACK_MAGIC;
    out->version = SML_TRANSPORT_ACK_VERSION;
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->reserved[2] = 0;
    out->sequence = sequence;
}

/* True when `data`/`len` is a well-formed ACK; on success `out_sequence` (may be
 * NULL) receives the acknowledged sequence. */
static inline bool sml_transport_ack_parse(const void *data, int len,
                                           uint32_t *out_sequence)
{
    if (data == NULL || len != (int)sizeof(sml_transport_ack_t)) {
        return false;
    }
    sml_transport_ack_t ack;
    __builtin_memcpy(&ack, data, sizeof(ack));
    if (ack.magic != SML_TRANSPORT_ACK_MAGIC ||
        ack.version != SML_TRANSPORT_ACK_VERSION) {
        return false;
    }
    if (out_sequence != NULL) {
        *out_sequence = ack.sequence;
    }
    return true;
}

/* Sender sleep-state announcement.
 *
 * The sender transmits this before the CPU parks for a light-sleep window and
 * again when it wakes, so the receiver always knows whether the sender is
 * reachable *right now* without having to poll for it. Packed into the same
 * ESP-NOW broadcast channel as the SML telegrams.
 *
 *   state  1 = about to sleep, 0 = awake (sent right after a real sleep)
 *   window_ms  the adaptive light-sleep window the sender is about to use
 *   sequence   monotonically increasing; lets the receiver tell duplicates
 */
#define SML_TRANSPORT_SLEEP_MAGIC 0x534C5031u /* "SLP1" */
#define SML_TRANSPORT_SLEEP_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t state;        /* 1 = sleeping (about to), 0 = awake */
    uint32_t sequence;    /* monotonic */
    uint32_t window_ms;   /* adaptive sleep window */
    uint32_t uptime_ms;   /* sender uptime for correlation */
} sml_transport_sleep_state_t;
