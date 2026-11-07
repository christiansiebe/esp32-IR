/* Runtime test knobs for the firmware test console.
 *
 * Changed through the console ('set' / 'reset-config') and persisted in NVS, so
 * a value survives a reboot and an OTA (no reflash needed). A build with a
 * different ceiling clamps a restored value back into range; 'reset-config'
 * erases the stored blob and returns to the compile-time defaults. The
 * parsing/clamping core is pure C and host-tested (test_runtime_config.c);
 * applying the values to the hardware and the NVS glue live in the IDF side. */
#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CPU frequency range the SoC supports; the compile-time ceiling is the upper
 * bound and is enforced by the caller when applying.
 *
 * The DFS *floor* may go down to 40 MHz, but the DFS *ceiling* must stay at or
 * above 80 MHz: the Wi-Fi/ESP-NOW driver asserts ESP_PM_APB_FREQ_MAX, and a
 * max_freq below 80 makes esp_pm_configure()/the Wi-Fi task abort (the sender
 * crashed when 40 was pinned as the ceiling).
 *
 * The top clock is SoC-specific: the ESP32 classic reaches 240 MHz, the
 * ESP32-C6 (single-core RISC-V) only 160 MHz. A ceiling above the SoC maximum
 * makes esp_pm_configure() reject the config, so the bound tracks the target. */
#ifdef CONFIG_IDF_TARGET_ESP32C6
#define RUNTIME_CONFIG_CPU_MAX_MHZ 160u
#else
#define RUNTIME_CONFIG_CPU_MAX_MHZ 240u
#endif
#define RUNTIME_CONFIG_CPU_MIN_MHZ 40u
#define RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ 80u

/* Radio-side power knobs (receiver SoftAP). The receiver's idle draw is set by
 * the always-on Wi-Fi radio, not the CPU, so the useful levers are the transmit
 * power and how often the AP beacons. 0 means "leave the chip/IDF default
 * alone"; a positive value is clamped into the ranges below. TX power is in dBm
 * (esp_wifi_set_max_tx_power takes 0.25 dBm steps, so it is scaled on apply);
 * the beacon interval is in IDF time units (1 TU = 1024 us) and must be a
 * multiple of 100. */
#define RUNTIME_CONFIG_TXPOWER_MIN_DBM 2u
#define RUNTIME_CONFIG_TXPOWER_MAX_DBM 20u
#define RUNTIME_CONFIG_BEACON_MIN_TU 100u
#define RUNTIME_CONFIG_BEACON_MAX_TU 6000u

/* Receiver radio sleep (opt-in). The Wi-Fi modem parks between wake windows and
 * the ESP-NOW connectionless module is told how often to wake and for how long
 * it stays awake. 0 = leave the chip/IDF default (radio always on). The interval
 * is the "wake every N ms" lever; the window must be <= the interval or the
 * radio never sleeps. Only the ESP32-C6 (and C2/C3/S2/S3) actually honours the
 * ESP-NOW wake window - on the classic ESP32 the API is accepted but breaks the
 * link, which is why this is receiver-only and off by default. */
#define RUNTIME_CONFIG_WAKE_INTERVAL_MIN_MS 10u
#define RUNTIME_CONFIG_WAKE_INTERVAL_MAX_MS 10000u
#define RUNTIME_CONFIG_WAKE_WINDOW_MIN_MS 1u
#define RUNTIME_CONFIG_WAKE_WINDOW_MAX_MS 10000u

/* Wi-Fi 6 target wake time (iTWT), opt-in and receiver-only. Instead of waking
 * at every DTIM beacon the associated station negotiates a longer wake cadence
 * with the AP; the hardware only reached 0.07 W unassociated, so an associated
 * C6 that no longer wakes every beacon is the lever for the associated idle
 * draw. Off by default (the AP must support TWT; the FRITZ!Box 5530 Fiber does).
 * twt_wake_ms is the negotiated wake interval in milliseconds. */
#define RUNTIME_CONFIG_TWT_WAKE_MIN_MS 500u
#define RUNTIME_CONFIG_TWT_WAKE_MAX_MS 10000u
#define RUNTIME_CONFIG_TWT_WAKE_DEFAULT_MS 5000u

typedef struct {
    uint32_t cpu_mhz;      /* DFS ceiling (max_freq) */
    uint32_t cpu_min_mhz;  /* DFS floor (min_freq); == cpu_mhz pins the clock */
    bool light_sleep;      /* allow the adaptive light sleep */
    uint32_t espnow_retries; /* per-frame ESP-NOW send attempts */
    uint8_t wifi_txpower_dbm; /* Wi-Fi max TX power in dBm; 0 = chip default */
    uint16_t ap_beacon_tu;    /* SoftAP beacon interval in TU; 0 = IDF default */
    bool radio_save;          /* receiver: park the Wi-Fi modem between wakes */
    uint16_t wake_interval_ms;/* ESP-NOW connectionless wake interval; 0 = off */
    uint16_t wake_window_ms;  /* radio-awake time per wake interval; 0 = off */
    bool twt_enabled;         /* receiver: negotiate Wi-Fi 6 TWT with the AP */
    uint16_t twt_wake_ms;     /* iTWT wake interval in ms (500..10000) */
} runtime_config_t;

/* Persisted blob wire format (NVS glue in main.c does the nvs_set_blob /
 * nvs_get_blob around it; kept pure C so it is host-tested). v5 appends the
 * Wi-Fi 6 TWT knobs (twt_enabled, twt_wake_ms); the reserved trailing byte keeps
 * the 16-bit fields aligned for a future field. An old (shorter) or different-
 * version blob is the wrong size and is ignored by runtime_config_decode(), so
 * the caller keeps its defaults (TWT off).
 *
 * NOTE: the parallel feat/c6-ap-switch branch also introduces this blob helper
 * and bumps the version to 5 (18 bytes, appending ap_enabled). At merge time the
 * two layouts must be folded into one version; the field offsets up to [15] are
 * identical, so only the appended tail differs. */
#define RUNTIME_CONFIG_BLOB_VERSION 5u
#define RUNTIME_CONFIG_BLOB_SIZE 20u

/* Encode cfg into a RUNTIME_CONFIG_BLOB_SIZE-byte blob. No-op when the buffer is
 * NULL or too small. */
void runtime_config_encode(const runtime_config_t *cfg, uint8_t *blob, size_t blob_size);

/* Decode a blob into *cfg. Returns false (leaving *cfg untouched) when the blob
 * is NULL, the wrong size, or carries a different version, so an old blob is
 * ignored and the caller keeps its defaults. */
bool runtime_config_decode(const uint8_t *blob, size_t blob_size, runtime_config_t *cfg);

/* Compile-time defaults (light sleep mirrors CONFIG_ESPNOW_SENDER_LIGHT_SLEEP). */
runtime_config_t runtime_config_defaults(void);

/* Restore the defaults into *cfg and return them. */
runtime_config_t runtime_config_reset(runtime_config_t *cfg);

/* Parse a decimal MHz value and clamp it to [CPU_MIN, min(CPU_MAX, ceiling)].
 * Returns false (and leaves *out untouched) on non-numeric input or 0. */
bool runtime_config_parse_cpu_mhz(const char *text, uint32_t ceiling_mhz, uint32_t *out);

/* As above but with an explicit lower bound, so a ceiling can be held at the
 * Wi-Fi minimum (80 MHz) while a floor may still go down to 40 MHz. */
bool runtime_config_parse_cpu_mhz_min(const char *text, uint32_t ceiling_mhz,
                                      uint32_t floor_mhz, uint32_t *out);

/* Parse "on"/"off" (also 1/0, true/false) for a boolean knob. */
bool runtime_config_parse_bool(const char *text, bool *out);

/* Split a TWT wake interval (in microseconds) into the mantissa/exponent pair
 * the iTWT setup frame carries, where the interval is
 * wake_invl_mant * 2^wake_invl_expn [us]. The mantissa is chosen as large as
 * possible (up to 65535) with the exponent as small as needed so the interval
 * is representable; the returned pair may overshoot the request (never under)
 * because not every microsecond count is expressible. Returns false (and leaves
 * the outputs untouched) for a zero interval. Pure C, host-tested. */
bool runtime_config_twt_split(uint32_t wake_us, uint8_t *expn, uint16_t *mant);

/* False when the USB Serial/JTAG peripheral is the primary console (the
 * ESP32-C6, which has no USB-UART bridge). Light sleep powers that PHY down:
 * the /dev/ttyACM* console disappears and, because the light-sleep flag is
 * persisted, the next boot never re-enumerates the port -- the device becomes
 * unreachable over USB (no console, no flash). IDF offers no light-sleep
 * keep-alive for this peripheral (SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP is
 * not defined on the C6, IDF-6395), so light sleep is refused whenever this is
 * false. Modem/radio sleep (the radio-save knob) is unaffected. */
bool runtime_config_light_sleep_allowed(void);

/* Set one knob by name. Returns true when the name is known and the value was
 * valid. *cfg is only modified on success. */
bool runtime_config_set(runtime_config_t *cfg, const char *key, const char *value,
                        uint32_t ceiling_mhz, char *err, size_t err_size);

/* Clamp every field into the range the current build allows: the CPU ceiling to
 * [80, min(CPU_MAX_MHZ, ceiling_mhz)] snapped to a real DFS step, the floor to
 * [40, ceiling], and the retries to 0..10. Used when a persisted config is
 * loaded back, so an image built with a different ceiling cannot restore an
 * out-of-range clock. */
void runtime_config_clamp(runtime_config_t *cfg, uint32_t ceiling_mhz);

/* Render the current values into out. */
void runtime_config_render(const runtime_config_t *cfg, char *out, size_t out_size);

/* --- IDF glue (no-ops when the feature is compiled out) ---
 * Apply the CPU ceiling and light-sleep toggle to the running system. Returns
 * false when the PM API rejected the change (the old values stay in effect). */
bool runtime_config_apply(const runtime_config_t *cfg);

#endif /* RUNTIME_CONFIG_H */
