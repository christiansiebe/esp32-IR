#include "runtime_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host builds compile this file without the IDF-injected sdkconfig.h. */
#ifndef CONFIG_ESPNOW_SENDER_LIGHT_SLEEP
#define CONFIG_ESPNOW_SENDER_LIGHT_SLEEP 0
#endif
#ifndef CONFIG_ESPNOW_RECEIVER_LIGHT_SLEEP
#define CONFIG_ESPNOW_RECEIVER_LIGHT_SLEEP 0
#endif
#ifndef CONFIG_ESPNOW_CPU_MAX_MHZ
#define CONFIG_ESPNOW_CPU_MAX_MHZ 240
#endif

/* Snap a value to the nearest of the DFS steps the ESP32 supports; defined
 * below, declared here so the defaults can use it too. */
static uint32_t snap_cpu_mhz(uint32_t mhz);

runtime_config_t runtime_config_defaults(void)
{
    /* Each role mirrors its own compile-time light-sleep default, so the
     * persisted knob starts in step with the image and 'set sleep' only ever
     * overrides an explicit choice. */
#if CONFIG_ESPNOW_ROLE_SENDER
    const bool light_sleep_default = CONFIG_ESPNOW_SENDER_LIGHT_SLEEP;
#else
    const bool light_sleep_default = CONFIG_ESPNOW_RECEIVER_LIGHT_SLEEP;
#endif
    runtime_config_t cfg = {
        .light_sleep = light_sleep_default,
        .cpu_mhz = snap_cpu_mhz(CONFIG_ESPNOW_CPU_MAX_MHZ),
        .cpu_min_mhz = 40u,
        .espnow_retries = 3u,
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

/* Snap a value to the nearest of the four DFS steps the ESP32 supports. Values
 * in between are silently rounded by esp_pm_configure() (a request for 200 MHz
 * becomes 240), which made 'set cpu' look like it did nothing. 40 is a legal
 * DFS step but not a legal ceiling, so anything at or below 80 snaps to 80. */
static uint32_t snap_cpu_mhz(uint32_t mhz)
{
    static const uint32_t steps[] = {80u, 160u, 240u};
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
        if (!runtime_config_parse_bool(value, &next.light_sleep)) {
            snprintf(err, err_size, "sleep: expected on or off\n");
            return false;
        }
    } else if (strcmp(key, "retries") == 0 || strcmp(key, "espnow-retries") == 0) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || v > 10) {
            snprintf(err, err_size, "retries: expected 0..10\n");
            return false;
        }
        next.espnow_retries = (uint32_t)v;
    } else {
        snprintf(err, err_size,
                 "unknown key '%s' (cpu, freq, cpu-min, sleep, retries)\n", key);
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
    if (cfg->espnow_retries > 10u) {
        cfg->espnow_retries = 10u;
    }
}

void runtime_config_render(const runtime_config_t *cfg, char *out, size_t out_size)
{
    if (cfg == NULL || out == NULL || out_size == 0) {
        return;
    }
    snprintf(out, out_size,
             "cpu ceiling: %u MHz\n"
             "cpu floor: %u MHz\n"
             "light sleep: %s\n"
             "espnow retries: %u\n",
             (unsigned)cfg->cpu_mhz, (unsigned)cfg->cpu_min_mhz,
             cfg->light_sleep ? "on" : "off",
             (unsigned)cfg->espnow_retries);
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
        .light_sleep_enable = cfg->light_sleep,
    };
    return esp_pm_configure(&pm) == ESP_OK;
}

#else /* host build: no power management to apply */

bool runtime_config_apply(const runtime_config_t *cfg)
{
    (void)cfg;
    return true;
}

#endif
