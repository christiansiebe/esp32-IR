#include "runtime_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host builds compile this file without the IDF-injected sdkconfig.h. The
 * fallback mirrors the SoC top clock: the host test exercises the ESP32 classic
 * (240 MHz), the target is selected by CONFIG_IDF_TARGET_ESP32C6 in a real build. */
#ifndef CONFIG_ESPNOW_SENDER_LIGHT_SLEEP
#define CONFIG_ESPNOW_SENDER_LIGHT_SLEEP 0
#endif
#ifndef CONFIG_ESPNOW_CPU_MAX_MHZ
#define CONFIG_ESPNOW_CPU_MAX_MHZ RUNTIME_CONFIG_CPU_MAX_MHZ
#endif
/* Host builds and the classic ESP32 leave this unset; the ESP32-C6 selects the
 * USB Serial/JTAG peripheral as its console (sdkconfig.defaults.esp32c6). */
#ifndef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 0
#endif

bool runtime_config_light_sleep_allowed(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return false;
#else
    return true;
#endif
}

/* Snap a value to the nearest of the DFS steps the ESP32 supports; defined
 * below, declared here so the defaults can use it too. */
static uint32_t snap_cpu_mhz(uint32_t mhz);

runtime_config_t runtime_config_defaults(void)
{
    runtime_config_t cfg = {
#if CONFIG_ESPNOW_SENDER_LIGHT_SLEEP
        .light_sleep = true,
#else
        .light_sleep = false,
#endif
        .cpu_mhz = snap_cpu_mhz(CONFIG_ESPNOW_CPU_MAX_MHZ),
        .cpu_min_mhz = 40u,
        .espnow_retries = 3u,
        /* 0 keeps the chip/IDF default so the classic-ESP32 receiver build is
         * byte-for-byte unchanged; only an explicit 'set' opts into a value. */
        .wifi_txpower_dbm = 0u,
        .ap_beacon_tu = 0u,
        /* Inert until radio_save is switched on; 100/70 mirrors the sender's
         * proven wake-interval/window defaults. */
        .radio_save = false,
        .wake_interval_ms = 100u,
        .wake_window_ms = 70u,
        /* Wi-Fi 6 TWT is opt-in; the wake cadence is only used once enabled. */
        .twt_enabled = false,
        .twt_wake_ms = RUNTIME_CONFIG_TWT_WAKE_DEFAULT_MS,
        /* Modem sleep is the receiver default (matches the pre-knob behaviour);
         * 'max' is only reached through an explicit 'set ps max'. */
        .wifi_ps_mode = RUNTIME_CONFIG_PS_DEFAULT,
        .listen_interval = RUNTIME_CONFIG_LISTEN_DEFAULT,
    };
    return cfg;
}

runtime_config_t runtime_config_reset(runtime_config_t *cfg)
{
    runtime_config_t defaults = runtime_config_defaults();
    if (cfg != NULL) {
        *cfg = defaults;
    }
    return defaults;
}

bool runtime_config_parse_cpu_mhz(const char *text, uint32_t ceiling_mhz, uint32_t *out)
{
    return runtime_config_parse_cpu_mhz_min(text, ceiling_mhz,
                                            RUNTIME_CONFIG_CPU_MIN_MHZ, out);
}

bool runtime_config_parse_cpu_mhz_min(const char *text, uint32_t ceiling_mhz,
                                      uint32_t floor_mhz, uint32_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        return false;
    }
    uint32_t limit = ceiling_mhz;
    if (limit > RUNTIME_CONFIG_CPU_MAX_MHZ) {
        limit = RUNTIME_CONFIG_CPU_MAX_MHZ;
    }
    if (limit < floor_mhz) {
        limit = floor_mhz;
    }
    if (value < floor_mhz) {
        value = floor_mhz;
    }
    if (value > limit) {
        value = limit;
    }
    *out = (uint32_t)value;
    return true;
}

/* Snap a value to the nearest of the DFS steps the SoC supports. Values in
 * between are silently rounded by esp_pm_configure() (a request for 200 MHz
 * becomes 240 on the ESP32), which made 'set cpu' look like it did nothing. 40
 * is a legal DFS step but not a legal ceiling, so anything at or below 80 snaps
 * to 80.
 *
 * The step set is SoC-specific: the ESP32 classic offers 80/160/240 MHz, the
 * ESP32-C6 (single-core RISC-V) offers 80/120/160 MHz. A value above the top
 * step is clamped to it by the caller's ceiling, so the snap only has to cover
 * the reachable range. */
static uint32_t snap_cpu_mhz(uint32_t mhz)
{
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    static const uint32_t steps[] = {80u, 120u, 160u};
#else
    static const uint32_t steps[] = {80u, 160u, 240u};
#endif
    if (mhz <= steps[0]) {
        return steps[0];
    }
    uint32_t best = steps[0];
    for (size_t i = 1; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        if (mhz >= steps[i]) {
            best = steps[i];
        }
    }
    return best;
}

void runtime_config_encode(const runtime_config_t *cfg, uint8_t *blob, size_t blob_size)
{
    if (cfg == NULL || blob == NULL || blob_size < RUNTIME_CONFIG_BLOB_SIZE) {
        return;
    }
    memset(blob, 0, blob_size);
    blob[0] = (uint8_t)RUNTIME_CONFIG_BLOB_VERSION;
    blob[1] = cfg->light_sleep ? 1u : 0u;
    blob[2] = (uint8_t)cfg->cpu_mhz;
    blob[3] = (uint8_t)cfg->cpu_min_mhz;
    uint32_t retries = cfg->espnow_retries;
    memcpy(&blob[4], &retries, sizeof(retries));
    blob[8] = cfg->wifi_txpower_dbm;
    uint16_t beacon = cfg->ap_beacon_tu;
    memcpy(&blob[9], &beacon, sizeof(beacon));
    blob[11] = cfg->radio_save ? 1u : 0u;
    uint16_t interval = cfg->wake_interval_ms;
    memcpy(&blob[12], &interval, sizeof(interval));
    uint16_t window = cfg->wake_window_ms;
    memcpy(&blob[14], &window, sizeof(window));
    blob[16] = cfg->twt_enabled ? 1u : 0u;
    uint16_t twt_wake = cfg->twt_wake_ms;
    memcpy(&blob[17], &twt_wake, sizeof(twt_wake));
    blob[19] = cfg->wifi_ps_mode;
    uint16_t listen = cfg->listen_interval;
    memcpy(&blob[20], &listen, sizeof(listen));
}

bool runtime_config_decode(const uint8_t *blob, size_t blob_size, runtime_config_t *cfg)
{
    if (blob == NULL || cfg == NULL || blob_size != RUNTIME_CONFIG_BLOB_SIZE ||
        blob[0] != RUNTIME_CONFIG_BLOB_VERSION) {
        return false;
    }
    runtime_config_t loaded = runtime_config_defaults();
    loaded.light_sleep = blob[1] != 0;
    loaded.cpu_mhz = blob[2];
    loaded.cpu_min_mhz = blob[3];
    uint32_t retries = 0;
    memcpy(&retries, &blob[4], sizeof(retries));
    loaded.espnow_retries = retries;
    loaded.wifi_txpower_dbm = blob[8];
    uint16_t beacon = 0;
    memcpy(&beacon, &blob[9], sizeof(beacon));
    loaded.ap_beacon_tu = beacon;
    loaded.radio_save = blob[11] != 0;
    uint16_t interval = 0;
    memcpy(&interval, &blob[12], sizeof(interval));
    loaded.wake_interval_ms = interval;
    uint16_t window = 0;
    memcpy(&window, &blob[14], sizeof(window));
    loaded.wake_window_ms = window;
    loaded.twt_enabled = blob[16] != 0;
    uint16_t twt_wake = 0;
    memcpy(&twt_wake, &blob[17], sizeof(twt_wake));
    loaded.twt_wake_ms = twt_wake;
    loaded.wifi_ps_mode = blob[19];
    uint16_t listen = 0;
    memcpy(&listen, &blob[20], sizeof(listen));
    loaded.listen_interval = listen;
    *cfg = loaded;
    return true;
}

bool runtime_config_parse_bool(const char *text, bool *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcmp(text, "on") == 0 || strcmp(text, "1") == 0 ||
        strcmp(text, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "0") == 0 ||
        strcmp(text, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool runtime_config_parse_ps(const char *text, uint8_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "none") == 0 ||
        strcmp(text, "0") == 0) {
        *out = RUNTIME_CONFIG_PS_OFF;
        return true;
    }
    if (strcmp(text, "min") == 0 || strcmp(text, "1") == 0) {
        *out = RUNTIME_CONFIG_PS_MIN;
        return true;
    }
    if (strcmp(text, "max") == 0 || strcmp(text, "2") == 0) {
        *out = RUNTIME_CONFIG_PS_MAX;
        return true;
    }
    return false;
}

bool runtime_config_twt_split(uint32_t wake_us, uint8_t *expn, uint16_t *mant)
{
    if (expn == NULL || mant == NULL || wake_us == 0u) {
        return false;
    }
    /* interval = mant * 2^expn [us]. Prefer the largest mantissa that fits (so
     * the exponent, and therefore the rounding error, stays small): start with
     * the exact 16-bit mantissa and shift right while it overflows, rounding up
     * so the negotiated interval is never shorter than requested. */
    uint32_t e = 0u;
    uint32_t m = wake_us;
    while (m > 0xFFFFu) {
        m = (m + 1u) >> 1; /* round up on the shift */
        ++e;
        if (e > 31u) {
            return false; /* beyond the 5-bit exponent field */
        }
    }
    if (m == 0u) {
        m = 1u;
    }
    *expn = (uint8_t)e;
    *mant = (uint16_t)m;
    return true;
}

bool runtime_config_set(runtime_config_t *cfg, const char *key, const char *value,
                        uint32_t ceiling_mhz, char *err, size_t err_size)
{
    if (cfg == NULL || key == NULL || value == NULL) {
        return false;
    }
    runtime_config_t next = *cfg;
    if (strcmp(key, "cpu") == 0 || strcmp(key, "cpu-mhz") == 0) {
        /* The ceiling must stay >= 80 MHz: a lower max_freq aborts the Wi-Fi
         * task (the sender crashed when 40 was set as the ceiling). */
        if (!runtime_config_parse_cpu_mhz_min(value, ceiling_mhz,
                                              RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ,
                                              &next.cpu_mhz)) {
            snprintf(err, err_size, "cpu: expected a number of MHz\n");
            return false;
        }
        /* Only 80/160/240 exist; snap so the reported value is the real clock. */
        next.cpu_mhz = snap_cpu_mhz(next.cpu_mhz);
        /* Keep the floor at or below the ceiling; otherwise min > max and the
         * PM API rejects the whole config. */
        if (next.cpu_min_mhz > next.cpu_mhz) {
            next.cpu_min_mhz = next.cpu_mhz;
        }
    } else if (strcmp(key, "freq") == 0 || strcmp(key, "freq-mhz") == 0) {
        /* Pin the CPU: DFS is off and the clock stays at this value. The pin is
         * a ceiling, so it is held at >= 80 MHz like 'cpu'. */
        uint32_t mhz;
        if (!runtime_config_parse_cpu_mhz_min(value, ceiling_mhz,
                                              RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ,
                                              &mhz)) {
            snprintf(err, err_size, "freq: expected a number of MHz\n");
            return false;
        }
        next.cpu_mhz = snap_cpu_mhz(mhz);
        next.cpu_min_mhz = next.cpu_mhz;
    } else if (strcmp(key, "cpu-min") == 0 || strcmp(key, "cpu-min-mhz") == 0) {
        if (!runtime_config_parse_cpu_mhz(value, ceiling_mhz, &next.cpu_min_mhz)) {
            snprintf(err, err_size, "cpu-min: expected a number of MHz\n");
            return false;
        }
        if (next.cpu_min_mhz > next.cpu_mhz) {
            next.cpu_min_mhz = next.cpu_mhz;
        }
    } else if (strcmp(key, "sleep") == 0 || strcmp(key, "light-sleep") == 0) {
        bool want = false;
        if (!runtime_config_parse_bool(value, &want)) {
            snprintf(err, err_size, "sleep: expected on or off\n");
            return false;
        }
        if (want && !runtime_config_light_sleep_allowed()) {
            snprintf(err, err_size,
                     "sleep: unavailable on this build (the USB Serial/JTAG "
                     "console would be lost)\n");
            return false;
        }
        next.light_sleep = want;
    } else if (strcmp(key, "retries") == 0 || strcmp(key, "espnow-retries") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || v > RUNTIME_CONFIG_RETRIES_MAX) {
            snprintf(err, err_size, "retries: expected 0..%u\n",
                     (unsigned)RUNTIME_CONFIG_RETRIES_MAX);
            return false;
        }
        next.espnow_retries = (uint32_t)v;
    } else if (strcmp(key, "txpower") == 0 || strcmp(key, "wifi-txpower") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || v > RUNTIME_CONFIG_TXPOWER_MAX_DBM ||
            (v != 0 && v < RUNTIME_CONFIG_TXPOWER_MIN_DBM)) {
            snprintf(err, err_size,
                     "txpower: expected 0 (default) or %u..%u dBm\n",
                     (unsigned)RUNTIME_CONFIG_TXPOWER_MIN_DBM,
                     (unsigned)RUNTIME_CONFIG_TXPOWER_MAX_DBM);
            return false;
        }
        next.wifi_txpower_dbm = (uint8_t)v;
    } else if (strcmp(key, "beacon") == 0 || strcmp(key, "beacon-tu") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || v > RUNTIME_CONFIG_BEACON_MAX_TU ||
            (v != 0 && (v < RUNTIME_CONFIG_BEACON_MIN_TU || v % 100u != 0u))) {
            snprintf(err, err_size,
                     "beacon: expected 0 (default) or %u..%u TU (multiple of 100)\n",
                     (unsigned)RUNTIME_CONFIG_BEACON_MIN_TU,
                     (unsigned)RUNTIME_CONFIG_BEACON_MAX_TU);
            return false;
        }
        next.ap_beacon_tu = (uint16_t)v;
    } else if (strcmp(key, "radio-save") == 0 || strcmp(key, "radio-ps") == 0) {
        if (!runtime_config_parse_bool(value, &next.radio_save)) {
            snprintf(err, err_size, "radio-save: expected on or off\n");
            return false;
        }
    } else if (strcmp(key, "wake-interval") == 0 || strcmp(key, "wake") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0' ||
            v < RUNTIME_CONFIG_WAKE_INTERVAL_MIN_MS ||
            v > RUNTIME_CONFIG_WAKE_INTERVAL_MAX_MS) {
            snprintf(err, err_size, "wake-interval: expected %u..%u ms\n",
                     (unsigned)RUNTIME_CONFIG_WAKE_INTERVAL_MIN_MS,
                     (unsigned)RUNTIME_CONFIG_WAKE_INTERVAL_MAX_MS);
            return false;
        }
        next.wake_interval_ms = (uint16_t)v;
        /* Keep the window inside the interval so the radio really sleeps. */
        if (next.wake_window_ms > next.wake_interval_ms) {
            next.wake_window_ms = next.wake_interval_ms;
        }
    } else if (strcmp(key, "wake-window") == 0 || strcmp(key, "window") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0' ||
            v < RUNTIME_CONFIG_WAKE_WINDOW_MIN_MS ||
            v > RUNTIME_CONFIG_WAKE_WINDOW_MAX_MS) {
            snprintf(err, err_size, "wake-window: expected %u..%u ms\n",
                     (unsigned)RUNTIME_CONFIG_WAKE_WINDOW_MIN_MS,
                     (unsigned)RUNTIME_CONFIG_WAKE_WINDOW_MAX_MS);
            return false;
        }
        next.wake_window_ms = (uint16_t)v;
    } else if (strcmp(key, "twt") == 0 || strcmp(key, "twt-enabled") == 0) {
        if (!runtime_config_parse_bool(value, &next.twt_enabled)) {
            snprintf(err, err_size, "twt: expected on or off\n");
            return false;
        }
    } else if (strcmp(key, "twt-wake") == 0 || strcmp(key, "twt-wake-ms") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0' ||
            v < RUNTIME_CONFIG_TWT_WAKE_MIN_MS ||
            v > RUNTIME_CONFIG_TWT_WAKE_MAX_MS) {
            snprintf(err, err_size, "twt-wake: expected %u..%u ms\n",
                     (unsigned)RUNTIME_CONFIG_TWT_WAKE_MIN_MS,
                     (unsigned)RUNTIME_CONFIG_TWT_WAKE_MAX_MS);
            return false;
        }
        next.twt_wake_ms = (uint16_t)v;
    } else if (strcmp(key, "ps") == 0 || strcmp(key, "wifi-ps") == 0) {
        uint8_t mode = 0;
        if (!runtime_config_parse_ps(value, &mode)) {
            snprintf(err, err_size, "ps: expected off, min or max (0/1/2)\n");
            return false;
        }
        next.wifi_ps_mode = mode;
    } else if (strcmp(key, "listen") == 0 || strcmp(key, "listen-interval") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0' ||
            v < RUNTIME_CONFIG_LISTEN_MIN || v > RUNTIME_CONFIG_LISTEN_MAX) {
            snprintf(err, err_size, "listen: expected %u..%u beacon intervals\n",
                     (unsigned)RUNTIME_CONFIG_LISTEN_MIN,
                     (unsigned)RUNTIME_CONFIG_LISTEN_MAX);
            return false;
        }
        next.listen_interval = (uint16_t)v;
    } else {
        snprintf(err, err_size,
                 "unknown key '%s' (cpu, freq, cpu-min, sleep, retries, txpower, "
                 "beacon, radio-save, wake-interval, wake-window, twt, twt-wake, "
                 "ps, listen, wifi-ssid, wifi-pass, mqtt-uri, mqtt-prefix, "
                 "mqtt-user, mqtt-pass)\n",
                 key);
        return false;
    }
    *cfg = next;
    return true;
}

void runtime_config_clamp(runtime_config_t *cfg, uint32_t ceiling_mhz)
{
    if (cfg == NULL) {
        return;
    }
    uint32_t ceiling = ceiling_mhz;
    if (ceiling < RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ) {
        ceiling = RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ;
    }
    if (ceiling > RUNTIME_CONFIG_CPU_MAX_MHZ) {
        ceiling = RUNTIME_CONFIG_CPU_MAX_MHZ;
    }
    uint32_t max_mhz = snap_cpu_mhz(cfg->cpu_mhz);
    /* Snap first, then clamp into [80, ceiling]. A value below the ceiling is
     * legal and may be intentional (e.g. a pinned 80 MHz receiver); only a
     * value the build cannot go that low is raised to the 80 MHz Wi-Fi floor. */
    if (max_mhz < RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ) {
        max_mhz = RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ;
    }
    if (max_mhz > ceiling) {
        max_mhz = ceiling;
    }
    cfg->cpu_mhz = max_mhz;

    uint32_t min_mhz = cfg->cpu_min_mhz;
    if (min_mhz < RUNTIME_CONFIG_CPU_MIN_MHZ) {
        min_mhz = RUNTIME_CONFIG_CPU_MIN_MHZ;
    }
    if (min_mhz > max_mhz) {
        min_mhz = max_mhz;
    }
    cfg->cpu_min_mhz = min_mhz;
    /* A build whose console is the USB Serial/JTAG peripheral must never enter
     * light sleep (it would kill the port for good); force the persisted value
     * off so a blob written by an earlier build cannot brick the USB port. */
    if (!runtime_config_light_sleep_allowed()) {
        cfg->light_sleep = false;
    }
    if (cfg->espnow_retries > RUNTIME_CONFIG_RETRIES_MAX) {
        cfg->espnow_retries = RUNTIME_CONFIG_RETRIES_MAX;
    }
    if (cfg->wifi_txpower_dbm > RUNTIME_CONFIG_TXPOWER_MAX_DBM) {
        cfg->wifi_txpower_dbm = RUNTIME_CONFIG_TXPOWER_MAX_DBM;
    }
    if (cfg->ap_beacon_tu != 0u &&
        (cfg->ap_beacon_tu < RUNTIME_CONFIG_BEACON_MIN_TU ||
         cfg->ap_beacon_tu > RUNTIME_CONFIG_BEACON_MAX_TU ||
         cfg->ap_beacon_tu % 100u != 0u)) {
        cfg->ap_beacon_tu = 0u;
    }
    /* The wake interval is the master: clamp it first, then pull the window
     * inside it so the radio really sleeps (a window >= interval keeps it on).
     * The uint16 fields are already <= their maximum by construction, so only
     * the lower bound and the interval/window relation are enforced here. */
    if (cfg->wake_interval_ms != 0u &&
        cfg->wake_interval_ms < RUNTIME_CONFIG_WAKE_INTERVAL_MIN_MS) {
        cfg->wake_interval_ms = RUNTIME_CONFIG_WAKE_INTERVAL_MIN_MS;
    }
    if (cfg->wake_window_ms < RUNTIME_CONFIG_WAKE_WINDOW_MIN_MS) {
        cfg->wake_window_ms = RUNTIME_CONFIG_WAKE_WINDOW_MIN_MS;
    }
    if (cfg->wake_interval_ms != 0u && cfg->wake_window_ms > cfg->wake_interval_ms) {
        cfg->wake_window_ms = cfg->wake_interval_ms;
    }
    /* The TWT wake interval: a value below the minimum is raised (a shorter
     * cadence than the AP/DTIM allows is pointless). */
    if (cfg->twt_wake_ms < RUNTIME_CONFIG_TWT_WAKE_MIN_MS) {
        cfg->twt_wake_ms = RUNTIME_CONFIG_TWT_WAKE_MIN_MS;
    }
    /* An out-of-range PS mode falls back to the default (min modem); the listen
     * interval is clamped into the uint16 the association request carries. A
     * persisted 0 (IDF's "use the default") is raised to the explicit default so
     * 'config' always reports a concrete value. */
    if (cfg->wifi_ps_mode > RUNTIME_CONFIG_PS_MAX) {
        cfg->wifi_ps_mode = RUNTIME_CONFIG_PS_DEFAULT;
    }
    if (cfg->listen_interval < RUNTIME_CONFIG_LISTEN_MIN) {
        cfg->listen_interval = RUNTIME_CONFIG_LISTEN_MIN;
    }
}

void runtime_config_health_clear(runtime_config_health_t *health)
{
    if (health == NULL) {
        return;
    }
    health->error = false;
    health->key[0] = '\0';
    health->reason[0] = '\0';
    health->repairs = 0;
}

void runtime_config_health_note(runtime_config_health_t *health, const char *key,
                                const char *reason)
{
    if (health == NULL) {
        return;
    }
    health->error = true;
    snprintf(health->key, sizeof(health->key), "%s", key != NULL ? key : "");
    snprintf(health->reason, sizeof(health->reason), "%s",
             reason != NULL ? reason : "apply failed");
    ++health->repairs;
}

/* Set one field of *cfg back to its compile-time default. Returns false when the
 * key is unknown (the caller then repairs the whole config). */
static bool repair_field(runtime_config_t *cfg, const char *key)
{
    runtime_config_t defaults = runtime_config_defaults();
    if (strcmp(key, "cpu") == 0 || strcmp(key, "cpu-mhz") == 0 ||
        strcmp(key, "freq") == 0 || strcmp(key, "freq-mhz") == 0) {
        cfg->cpu_mhz = defaults.cpu_mhz;
    } else if (strcmp(key, "cpu-min") == 0 || strcmp(key, "cpu-min-mhz") == 0) {
        cfg->cpu_min_mhz = defaults.cpu_min_mhz;
    } else if (strcmp(key, "sleep") == 0 || strcmp(key, "light-sleep") == 0) {
        cfg->light_sleep = defaults.light_sleep;
    } else if (strcmp(key, "retries") == 0 || strcmp(key, "espnow-retries") == 0) {
        cfg->espnow_retries = defaults.espnow_retries;
    } else if (strcmp(key, "txpower") == 0 || strcmp(key, "wifi-txpower") == 0) {
        cfg->wifi_txpower_dbm = defaults.wifi_txpower_dbm;
    } else if (strcmp(key, "beacon") == 0 || strcmp(key, "beacon-tu") == 0) {
        cfg->ap_beacon_tu = defaults.ap_beacon_tu;
    } else if (strcmp(key, "radio-save") == 0 || strcmp(key, "radio-ps") == 0) {
        cfg->radio_save = defaults.radio_save;
    } else if (strcmp(key, "wake-interval") == 0 || strcmp(key, "wake") == 0) {
        cfg->wake_interval_ms = defaults.wake_interval_ms;
    } else if (strcmp(key, "wake-window") == 0 || strcmp(key, "window") == 0) {
        cfg->wake_window_ms = defaults.wake_window_ms;
    } else if (strcmp(key, "twt") == 0 || strcmp(key, "twt-enabled") == 0) {
        cfg->twt_enabled = defaults.twt_enabled;
    } else if (strcmp(key, "twt-wake") == 0 || strcmp(key, "twt-wake-ms") == 0) {
        cfg->twt_wake_ms = defaults.twt_wake_ms;
    } else if (strcmp(key, "ps") == 0 || strcmp(key, "wifi-ps") == 0) {
        cfg->wifi_ps_mode = defaults.wifi_ps_mode;
    } else if (strcmp(key, "listen") == 0 || strcmp(key, "listen-interval") == 0) {
        cfg->listen_interval = defaults.listen_interval;
    } else {
        return false;
    }
    return true;
}

bool runtime_config_repair(runtime_config_t *cfg, const char *key,
                           const char *reason, runtime_config_health_t *health)
{
    if (cfg == NULL) {
        return false;
    }
    runtime_config_t defaults = runtime_config_defaults();
    if (key == NULL || strcmp(key, "config") == 0 || !repair_field(cfg, key)) {
        *cfg = defaults;
        runtime_config_health_note(health, key != NULL ? key : "config",
                                   reason != NULL ? reason : "apply failed");
        return true;
    }
    runtime_config_health_note(health, key, reason != NULL ? reason : "apply failed");
    return true;
}

bool runtime_config_verify(const runtime_config_t *cfg, int observed_ps,
                           int observed_pm_max_mhz, runtime_config_health_t *health)
{
    if (cfg == NULL) {
        return true;
    }
    /* The modem PS mode is only observable once the Wi-Fi driver is up; -1 means
     * "not available" and is not a mismatch. A SoftAP receiver keeps the driver
     * in APSTA, so get_ps() is meaningful there too. */
    if (observed_ps >= 0 && observed_ps != (int)cfg->wifi_ps_mode) {
        runtime_config_health_note(health, "ps", "esp_wifi_get_ps() disagreed");
        return false;
    }
    /* esp_pm_get_configuration() reports the configured ceiling, not the live
     * (DFS-stepped) clock, so a mismatch means the config never took effect. 0
     * means PM is not configured (e.g. the host build) and is skipped. */
    if (observed_pm_max_mhz > 0 &&
        observed_pm_max_mhz != (int)cfg->cpu_mhz) {
        runtime_config_health_note(health, "cpu", "esp_pm_get_configuration() disagreed");
        return false;
    }
    return true;
}

void runtime_config_health_render(const runtime_config_health_t *health, char *out,
                                  size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (health == NULL || !health->error) {
        return;
    }
    if (health->key[0] != '\0') {
        snprintf(out, out_size, "last apply error: %s [%s] (repaired to default)\n",
                 health->reason, health->key);
    } else {
        snprintf(out, out_size, "last apply error: %s (repaired to default)\n",
                 health->reason);
    }
}

void runtime_config_render(const runtime_config_t *cfg, char *out, size_t out_size)
{
    if (cfg == NULL || out == NULL || out_size == 0) {
        return;
    }
    char tx[24];
    char beacon[24];
    if (cfg->wifi_txpower_dbm == 0u) {
        snprintf(tx, sizeof(tx), "chip default");
    } else {
        snprintf(tx, sizeof(tx), "%u dBm", (unsigned)cfg->wifi_txpower_dbm);
    }
    if (cfg->ap_beacon_tu == 0u) {
        snprintf(beacon, sizeof(beacon), "idf default");
    } else {
        snprintf(beacon, sizeof(beacon), "%u TU", (unsigned)cfg->ap_beacon_tu);
    }
    char wake[32];
    if (!cfg->radio_save) {
        snprintf(wake, sizeof(wake), "off");
    } else {
        snprintf(wake, sizeof(wake), "on (%u ms / %u ms window)",
                 (unsigned)cfg->wake_interval_ms, (unsigned)cfg->wake_window_ms);
    }
    char twt[40];
    if (!cfg->twt_enabled) {
        snprintf(twt, sizeof(twt), "off");
    } else {
        uint8_t expn = 0;
        uint16_t mant = 0;
        if (runtime_config_twt_split((uint32_t)cfg->twt_wake_ms * 1000u, &expn, &mant)) {
            snprintf(twt, sizeof(twt), "on (%u ms = mant %u * 2^%u)",
                     (unsigned)cfg->twt_wake_ms, (unsigned)mant, (unsigned)expn);
        } else {
            snprintf(twt, sizeof(twt), "on (%u ms)", (unsigned)cfg->twt_wake_ms);
        }
    }
    static const char *const ps_names[] = {"off", "min", "max"};
    const char *ps = cfg->wifi_ps_mode <= RUNTIME_CONFIG_PS_MAX
                         ? ps_names[cfg->wifi_ps_mode]
                         : "min";
    snprintf(out, out_size,
             "cpu ceiling: %u MHz\n"
             "cpu floor: %u MHz\n"
             "light sleep: %s\n"
             "espnow retries: %u\n"
             "wifi txpower: %s\n"
             "ap beacon: %s\n"
             "radio save: %s\n"
             "twt: %s\n"
             "wifi ps: %s\n"
             "listen interval: %u beacons\n",
             (unsigned)cfg->cpu_mhz, (unsigned)cfg->cpu_min_mhz,
             cfg->light_sleep ? "on"
                              : (runtime_config_light_sleep_allowed()
                                     ? "off"
                                     : "off (unavailable: USB Serial/JTAG console)"),
             (unsigned)cfg->espnow_retries, tx, beacon, wake, twt, ps,
             (unsigned)cfg->listen_interval);
}

#ifdef ESP_PLATFORM

#include "esp_pm.h"

bool runtime_config_apply(const runtime_config_t *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    /* Changing max_freq_mhz reconfigures DFS; when min_freq == max_freq DFS is
     * effectively off and the clock is pinned. light_sleep_enable toggles the
     * idle park. */
    /* Safety net: never hand esp_pm_configure() a ceiling below the Wi-Fi
     * minimum. A 40 MHz max_freq aborts the Wi-Fi task and crashes the sender,
     * so clamp here too in case a caller bypasses the parser. */
    int max_mhz = (int)cfg->cpu_mhz;
    if (max_mhz < (int)RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ) {
        max_mhz = (int)RUNTIME_CONFIG_CPU_CEILING_MIN_MHZ;
    }
    int min_mhz = (int)cfg->cpu_min_mhz;
    if (min_mhz < (int)RUNTIME_CONFIG_CPU_MIN_MHZ) {
        min_mhz = (int)RUNTIME_CONFIG_CPU_MIN_MHZ;
    }
    if (min_mhz > max_mhz) {
        min_mhz = max_mhz;
    }
    esp_pm_config_t pm = {
        .max_freq_mhz = max_mhz,
        .min_freq_mhz = min_mhz,
        /* Never hand light sleep to a build whose console is the USB
         * Serial/JTAG peripheral: the PHY is powered down in light sleep and the
         * port would never come back (IDF-6395). */
        .light_sleep_enable = cfg->light_sleep && runtime_config_light_sleep_allowed(),
    };
    if (esp_pm_configure(&pm) != ESP_OK) {
        return false;
    }
    /* Self-check: read the configured ceiling back. esp_pm_get_configuration()
     * reports the configured max_freq, so a mismatch means the request never
     * took effect; returning false lets the caller repair to the default. */
    esp_pm_config_t observed = {0};
    if (esp_pm_get_configuration(&observed) == ESP_OK &&
        observed.max_freq_mhz != max_mhz) {
        return false;
    }
    return true;
}

#else /* host build: no power management to apply */

bool runtime_config_apply(const runtime_config_t *cfg)
{
    (void)cfg;
    return true;
}

#endif
