#include "bssid_pin.h"

#include <string.h>

void bssid_pin_init(bssid_pin_t *pin, uint32_t failopen_ms)
{
    memset(pin, 0, sizeof(*pin));
    pin->failopen_ms = failopen_ms;
}

void bssid_pin_set(bssid_pin_t *pin, const uint8_t *bssid, uint64_t now_ms)
{
    if (bssid == NULL) {
        return;
    }
    memcpy(pin->bssid, bssid, BSSID_PIN_MAC_LEN);
    pin->valid = true;
    pin->armed_since_ms = now_ms;
    ++pin->pins;
}

void bssid_pin_restore(bssid_pin_t *pin, const uint8_t *bssid, uint64_t now_ms)
{
    if (bssid == NULL) {
        return;
    }
    memcpy(pin->bssid, bssid, BSSID_PIN_MAC_LEN);
    pin->valid = true;
    pin->armed_since_ms = now_ms;
}

void bssid_pin_clear(bssid_pin_t *pin)
{
    pin->valid = false;
}

bool bssid_pin_note_connected(bssid_pin_t *pin, const uint8_t *bssid,
                              bool allow_new_pin, uint64_t now_ms)
{
    if (pin->valid) {
        /* The pin holds; restart the fail-open clock so it never expires while
         * the association keeps succeeding. */
        pin->armed_since_ms = now_ms;
        return false;
    }
    if (!allow_new_pin || bssid == NULL) {
        return false;
    }
    memcpy(pin->bssid, bssid, BSSID_PIN_MAC_LEN);
    pin->valid = true;
    pin->armed_since_ms = now_ms;
    ++pin->pins;
    return true;
}

bool bssid_pin_note_disconnect(bssid_pin_t *pin, bool bssid_unreachable,
                               uint64_t now_ms)
{
    if (!pin->valid) {
        return false;
    }
    if (bssid_unreachable) {
        /* The pinned BSSID is demonstrably gone: release at once rather than
         * hold the receiver offline waiting out the window. */
        pin->valid = false;
        ++pin->releases;
        return true;
    }
    /* Tolerate a clock that has not advanced (a disconnect in the same tick as
     * the pin) without releasing early. */
    if (now_ms < pin->armed_since_ms) {
        pin->armed_since_ms = now_ms;
        return false;
    }
    if (now_ms - pin->armed_since_ms < pin->failopen_ms) {
        return false;
    }
    pin->valid = false;
    ++pin->releases;
    return true;
}

bool bssid_pin_get(const bssid_pin_t *pin, uint8_t *out)
{
    if (!pin->valid || out == NULL) {
        return false;
    }
    memcpy(out, pin->bssid, BSSID_PIN_MAC_LEN);
    return true;
}
