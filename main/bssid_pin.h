#pragma once

/* Sticky BSSID pin with fail-open (receiver).
 *
 * A Wi-Fi mesh publishes one SSID across several BSSIDs on different channels.
 * A station with a weak signal (-84..-89 dBm) is handed back and forth between
 * them, and every handover moves the radio channel. ESP-NOW is channel-bound:
 * the receiver's peers latch the channel they were added on, so the handover
 * silently kills the sender link (the "187 s cadence" bug) even though the
 * sender never stopped. Re-anchoring the peers heals it (see main.c
 * STA_CONNECTED), but the handover itself keeps costing frames.
 *
 * This module removes the cause instead of healing it: it remembers the BSSID
 * the station first associated with and pins to it, so the mesh can no longer
 * move the station (and the ESP-NOW channel) around.
 *
 * Fail-open is the safety net that matches the operating priority "the receiver
 * must never go offline": if the pinned BSSID disappears (the AP was switched
 * off, the mesh reconfigured), the pin is released after `failopen_ms` without
 * a successful association and the next connect is free to pick any BSSID. The
 * device therefore never wedges itself onto a BSSID that no longer answers -
 * losing the sender link is acceptable, losing the receiver is not.
 *
 * Pure C, no ESP-IDF, so the decision is host-tested (test/test_bssid_pin.c);
 * main.c performs the association feed, the NVS persistence and the reconnect.
 */

#include <stdbool.h>
#include <stdint.h>

#define BSSID_PIN_MAC_LEN 6

typedef struct {
    /* Configuration. */
    uint32_t failopen_ms;   /* release the pin after this long without a connect */

    /* Observation state. */
    bool valid;             /* a BSSID is currently pinned */
    uint8_t bssid[BSSID_PIN_MAC_LEN];
    uint64_t armed_since_ms; /* when the pin was (re)armed: last pin or connect */
    uint32_t pins;          /* pins taken since boot (auto or manual) */
    uint32_t releases;      /* fail-open releases since boot */
} bssid_pin_t;

/* Fail-open default: long enough that a slow re-association (scan + auth) does
 * not release a still-valid pin, short enough that a genuinely gone AP costs
 * only a minute of sender link. */
#define BSSID_PIN_DEFAULT_FAILOPEN_MS (60u * 1000u)

void bssid_pin_init(bssid_pin_t *pin, uint32_t failopen_ms);

/* Take a pin explicitly (console 'set bssid <mac>' / a persisted pin at boot).
 * Overwrites any existing pin and arms the fail-open timer at now_ms. */
void bssid_pin_set(bssid_pin_t *pin, const uint8_t *bssid, uint64_t now_ms);

/* Restore a persisted pin at boot: like bssid_pin_set() but it does not count as
 * a new pin and arms the fail-open clock at now_ms (not at 0, which would let the
 * first disconnect release a pin that was only just restored). */
void bssid_pin_restore(bssid_pin_t *pin, const uint8_t *bssid, uint64_t now_ms);

/* Drop the pin (console 'set bssid off'). */
void bssid_pin_clear(bssid_pin_t *pin);

/* Feed a successful association.
 *  - With a pin held: re-arm the fail-open timer (the pin just proved itself).
 *  - With no pin and allow_new_pin: take the pin on `bssid`.
 * Returns true when a NEW pin was taken, so the caller can persist it. */
bool bssid_pin_note_connected(bssid_pin_t *pin, const uint8_t *bssid,
                              bool allow_new_pin, uint64_t now_ms);

/* Feed a disconnect.
 *  - bssid_unreachable: the driver reported the pinned BSSID itself is gone
 *    (no AP found). The pin is released immediately: waiting the full window
 *    while the AP is demonstrably absent would keep the receiver offline for
 *    no reason, and availability beats the sender link.
 *  - otherwise: release only once the pinned BSSID has been gone for
 *    failopen_ms, so a transient drop does not undo the pin.
 * A no-op (returns false) when no pin is held. */
bool bssid_pin_note_disconnect(bssid_pin_t *pin, bool bssid_unreachable,
                               uint64_t now_ms);

/* Copy the pinned BSSID into out (6 bytes). Returns false when none is held. */
bool bssid_pin_get(const bssid_pin_t *pin, uint8_t *out);
