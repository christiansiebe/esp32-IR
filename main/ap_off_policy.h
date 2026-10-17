/* Pure decision for the "drop the SoftAP once the station is up" power saving
 * (receiver, APSTA mode). Kept out of main.c so the exact rule is host-tested.
 *
 * The receiver keeps its "ESP32-IR-Meter" SoftAP until the station is actually
 * up, so a misconfigured home WLAN still has the 192.168.4.1 fallback. Once the
 * home WLAN carries the dashboard the AP is idle radio cost and is switched off.
 *
 * The one hard constraint: a sender that boots *after* us finds the ESP-NOW
 * channel through the receiver's AP and only falls back to CONFIG_ESPNOW_CHANNEL
 * when the AP is gone. So the AP may be dropped only when the station's channel
 * equals ESPNOW_CHANNEL; on any other channel a late sender would never align. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* True when the receiver may turn its SoftAP off right now. `sta_channel` is
 * the channel the station is associated on, `espnow_channel` the configured
 * ESP-NOW channel (the sender's fallback). The AP is dropped only when both
 * match, so the sender's channel discovery cannot break. */
static inline bool ap_off_allowed_on_channel(uint8_t sta_channel,
                                             uint8_t espnow_channel)
{
    return sta_channel != 0 && sta_channel == espnow_channel;
}

/* Debounce for the APSTA<->STA mode switch. The switch itself can emit a
 * STA_DISCONNECTED event; reacting to it would immediately turn the AP back on
 * and flap. True when the last switch is old enough to trust a disconnect as
 * real. `since_switch_us` is esp_timer_get_time() - last_switch_us. */
static inline bool ap_off_disconnect_is_real(int64_t since_switch_us)
{
    return since_switch_us > 5 * 1000 * 1000;
}
