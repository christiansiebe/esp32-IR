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

/* CPU frequency range the ESP32-WROOM-32D supports; the compile-time ceiling is
 * the upper bound and is enforced by the caller when applying.
 *
 * The DFS *floor* may go down to 40 MHz, but the DFS *ceiling* must stay at or
 * above 80 MHz: the Wi-Fi/ESP-NOW driver asserts ESP_PM_APB_FREQ_MAX, and a
 * max_freq below 80 makes esp_pm_configure()/the Wi-Fi task abort (the sender
 * crashed when 40 was pinned as the ceiling). */
#define RUNTIME_CONFIG_CPU_MIN_MHZ 40u
#define RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ 80u
#define RUNTIME_CONFIG_CPU_MAX_MHZ 240u

typedef struct {
    uint32_t cpu_mhz;      /* DFS ceiling (max_freq) */
    uint32_t cpu_min_mhz;  /* DFS floor (min_freq); == cpu_mhz pins the clock */
    bool light_sleep;      /* allow the adaptive light sleep */
    uint32_t espnow_retries; /* per-frame ESP-NOW send attempts */
} runtime_config_t;

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

/* Set one knob by name. Returns true when the name is known and the value was
 * valid. *cfg is only modified on success. */
bool runtime_config_set(runtime_config_t *cfg, const char *key, const char *value,
                        uint32_t ceiling_mhz, char *err, size_t err_size);

/* Clamp every field into the range the current build allows: the CPU ceiling to
 * [80, min(240, ceiling_mhz)] snapped to a real DFS step, the floor to
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
