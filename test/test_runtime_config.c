/* Host test for the runtime test knobs (main/runtime_config.c).
 *
 * These back the firmware test console's `set` / `reset-config` commands, so a
 * bad parse must never silently change the DFS ceiling or light-sleep flag used
 * by a bench run. Pure C, no IDF.
 */
#include <stdio.h>
#include <string.h>

#include "runtime_config.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

int main(void)
{
    runtime_config_t cfg = runtime_config_defaults();
    CHECK(cfg.cpu_mhz >= RUNTIME_CONFIG_CPU_MIN_MHZ &&
              cfg.cpu_mhz <= RUNTIME_CONFIG_CPU_MAX_MHZ,
          "default cpu ceiling is within range");

    /* CPU MHz clamps into [40, min(240, ceiling)]. */
    uint32_t v = 0;
    CHECK(runtime_config_parse_cpu_mhz("80", 240, &v) && v == 80,
          "80 MHz parses");
    CHECK(runtime_config_parse_cpu_mhz("10", 240, &v) && v == 40,
          "below the floor clamps to 40");
    CHECK(runtime_config_parse_cpu_mhz("400", 240, &v) && v == 240,
          "above the ceiling clamps to the ceiling");
    CHECK(runtime_config_parse_cpu_mhz("400", 160, &v) && v == 160,
          "a lower compile-time ceiling wins");
    CHECK(!runtime_config_parse_cpu_mhz("abc", 240, &v), "non-numeric is rejected");
    CHECK(!runtime_config_parse_cpu_mhz("0", 240, &v), "zero is rejected");
    /* The explicit-floor variant holds a ceiling at the Wi-Fi minimum. */
    CHECK(runtime_config_parse_cpu_mhz_min("40", 240, 80, &v) && v == 80,
          "ceiling clamped up to the 80 MHz Wi-Fi minimum");
    CHECK(runtime_config_parse_cpu_mhz_min("80", 240, 80, &v) && v == 80,
          "80 MHz is accepted as a ceiling");

    /* Booleans. */
    bool b = false;
    CHECK(runtime_config_parse_bool("on", &b) && b, "'on' is true");
    CHECK(runtime_config_parse_bool("off", &b) && !b, "'off' is false");
    CHECK(runtime_config_parse_bool("1", &b) && b, "'1' is true");
    CHECK(!runtime_config_parse_bool("maybe", &b), "'maybe' is rejected");

    /* set(): known keys apply, unknown/invalid leave the struct untouched. */
    char err[64];
    runtime_config_t before = cfg;
    CHECK(runtime_config_set(&cfg, "cpu", "80", 240, err, sizeof(err)) &&
              cfg.cpu_mhz == 80,
          "set cpu=80");
    CHECK(runtime_config_set(&cfg, "sleep", "off", 240, err, sizeof(err)) &&
              !cfg.light_sleep,
          "set sleep=off");
    CHECK(runtime_config_set(&cfg, "retries", "5", 240, err, sizeof(err)) &&
              cfg.espnow_retries == 5,
          "set retries=5");

    /* freq pins the clock: floor == ceiling (DFS off). */
    CHECK(runtime_config_set(&cfg, "freq", "80", 240, err, sizeof(err)) &&
              cfg.cpu_mhz == 80 && cfg.cpu_min_mhz == 80,
          "set freq=80 pins the clock");
    /* A ceiling below the Wi-Fi minimum (80 MHz) is clamped up: 40 as a
     * max_freq aborts the Wi-Fi task. */
    CHECK(runtime_config_set(&cfg, "cpu", "40", 240, err, sizeof(err)) &&
              cfg.cpu_mhz == 80 && cfg.cpu_min_mhz == 80,
          "a ceiling below 80 MHz clamps up to 80");
    CHECK(runtime_config_set(&cfg, "freq", "40", 240, err, sizeof(err)) &&
              cfg.cpu_mhz == 80 && cfg.cpu_min_mhz == 80,
          "pinning 40 MHz clamps up to the 80 MHz Wi-Fi minimum");
    /* Only 80/160/240 exist; values in between snap so the reported clock is
     * the real one (esp_pm_configure() would silently round anyway). */
    CHECK(runtime_config_set(&cfg, "cpu", "100", 240, err, sizeof(err)) &&
              cfg.cpu_mhz == 80,
          "a ceiling between steps snaps down to 80");
    CHECK(runtime_config_set(&cfg, "cpu", "200", 240, err, sizeof(err)) &&
              cfg.cpu_mhz == 160,
          "a ceiling between steps snaps down to 160");
    CHECK(runtime_config_set(&cfg, "freq", "100", 240, err, sizeof(err)) &&
              cfg.cpu_mhz == 80 && cfg.cpu_min_mhz == 80,
          "pinning 100 MHz snaps to 80");
    /* cpu-min raises the floor without changing the ceiling. */
    CHECK(runtime_config_set(&cfg, "freq", "160", 240, err, sizeof(err)) &&
              cfg.cpu_mhz == 160 && cfg.cpu_min_mhz == 160,
          "set freq=160");
    CHECK(runtime_config_set(&cfg, "cpu-min", "80", 240, err, sizeof(err)) &&
              cfg.cpu_min_mhz == 80 && cfg.cpu_mhz == 160,
          "set cpu-min=80 keeps the ceiling");
    CHECK(runtime_config_set(&cfg, "cpu-min", "240", 240, err, sizeof(err)) &&
              cfg.cpu_min_mhz == 160,
          "cpu-min cannot exceed the ceiling");
    before = cfg;
    CHECK(!runtime_config_set(&cfg, "freq", "abc", 240, err, sizeof(err)),
          "non-numeric freq is rejected");
    CHECK(cfg.cpu_mhz == before.cpu_mhz && cfg.cpu_min_mhz == before.cpu_min_mhz,
          "a rejected freq leaves the config unchanged");
    before = cfg;
    CHECK(!runtime_config_set(&cfg, "bogus", "1", 240, err, sizeof(err)),
          "unknown key is rejected");
    CHECK(cfg.cpu_mhz == before.cpu_mhz && cfg.light_sleep == before.light_sleep &&
              cfg.espnow_retries == before.espnow_retries,
          "a rejected set leaves the config unchanged");
    CHECK(!runtime_config_set(&cfg, "retries", "99", 240, err, sizeof(err)),
          "out-of-range retries rejected");

    /* clamp(): a value restored from NVS is forced into what this build allows.
     * This is the guard that lets a persisted 'set' survive an OTA into an image
     * with a different ceiling. */
    runtime_config_t stored = {.cpu_mhz = 240, .cpu_min_mhz = 40,
                               .light_sleep = false, .espnow_retries = 3};
    runtime_config_clamp(&stored, 80);
    CHECK(stored.cpu_mhz == 80 && stored.cpu_min_mhz == 40,
          "a 240 MHz value restored into an 80 MHz build clamps to 80");
    stored.cpu_mhz = 240;
    stored.cpu_min_mhz = 240;
    runtime_config_clamp(&stored, 80);
    CHECK(stored.cpu_mhz == 80 && stored.cpu_min_mhz == 80,
          "a pinned 240 MHz value clamps the floor down with the ceiling");
    stored.cpu_mhz = 200; /* not a DFS step */
    stored.cpu_min_mhz = 0;
    runtime_config_clamp(&stored, 240);
    CHECK(stored.cpu_mhz == 160 && stored.cpu_min_mhz == 40,
          "a non-step value snaps and the floor is raised to 40");
    stored.cpu_mhz = 0;
    stored.cpu_min_mhz = 0;
    runtime_config_clamp(&stored, 240);
    CHECK(stored.cpu_mhz == 80, "a zeroed value clamps up to the 80 MHz minimum");
    stored.cpu_mhz = 240;
    stored.cpu_min_mhz = 40;
    stored.espnow_retries = 99;
    runtime_config_clamp(&stored, 240);
    CHECK(stored.espnow_retries == 10, "retries above 10 clamp to 10");

    /* reset restores the defaults. */
    cfg = runtime_config_reset(&cfg);
    runtime_config_t defaults = runtime_config_defaults();
    CHECK(cfg.cpu_mhz == defaults.cpu_mhz &&
              cfg.cpu_min_mhz == defaults.cpu_min_mhz &&
              cfg.light_sleep == defaults.light_sleep,
          "reset restores the defaults");

    printf("runtime config: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
