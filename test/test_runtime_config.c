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

    /* Radio-side power knobs. Default 0 means "leave the chip/IDF default". */
    cfg = runtime_config_defaults();
    CHECK(cfg.wifi_txpower_dbm == 0 && cfg.ap_beacon_tu == 0,
          "radio power knobs default to the chip/IDF default");
    CHECK(runtime_config_set(&cfg, "txpower", "8", 240, err, sizeof(err)) &&
              cfg.wifi_txpower_dbm == 8,
          "set txpower=8 dBm");
    CHECK(runtime_config_set(&cfg, "txpower", "0", 240, err, sizeof(err)) &&
              cfg.wifi_txpower_dbm == 0,
          "txpower 0 restores the chip default");
    CHECK(!runtime_config_set(&cfg, "txpower", "1", 240, err, sizeof(err)),
          "txpower below 2 dBm is rejected (chip minimum)");
    CHECK(!runtime_config_set(&cfg, "txpower", "21", 240, err, sizeof(err)),
          "txpower above 20 dBm is rejected");
    CHECK(runtime_config_set(&cfg, "beacon", "1000", 240, err, sizeof(err)) &&
              cfg.ap_beacon_tu == 1000,
          "set beacon=1000 TU");
    CHECK(runtime_config_set(&cfg, "beacon", "0", 240, err, sizeof(err)) &&
              cfg.ap_beacon_tu == 0,
          "beacon 0 restores the IDF default");
    CHECK(!runtime_config_set(&cfg, "beacon", "150", 240, err, sizeof(err)),
          "a beacon interval not a multiple of 100 TU is rejected");
    CHECK(!runtime_config_set(&cfg, "beacon", "50", 240, err, sizeof(err)),
          "a beacon interval below 100 TU is rejected");
    /* clamp() drops an out-of-range radio knob back to the default. */
    runtime_config_t stored = {.cpu_mhz = 240, .cpu_min_mhz = 40,
                               .light_sleep = false, .espnow_retries = 3};
    stored.wifi_txpower_dbm = 99;
    stored.ap_beacon_tu = 150;
    runtime_config_clamp(&stored, 240);
    CHECK(stored.wifi_txpower_dbm == RUNTIME_CONFIG_TXPOWER_MAX_DBM,
          "txpower above the max clamps to the max");
    CHECK(stored.ap_beacon_tu == 0,
          "an off-grid beacon interval resets to the IDF default");

    /* Receiver radio-sleep knobs. radio_save is a bool; wake interval/window
     * are in ms and the window is pulled inside the interval. */
    cfg = runtime_config_defaults();
    CHECK(!cfg.radio_save, "radio-save is off by default");
    CHECK(runtime_config_set(&cfg, "radio-save", "on", 240, err, sizeof(err)) &&
              cfg.radio_save,
          "set radio-save=on");
    CHECK(runtime_config_set(&cfg, "wake-interval", "500", 240, err, sizeof(err)) &&
              cfg.wake_interval_ms == 500,
          "set wake-interval=500 ms");
    CHECK(runtime_config_set(&cfg, "wake-window", "200", 240, err, sizeof(err)) &&
              cfg.wake_window_ms == 200,
          "set wake-window=200 ms");
    /* Raising the window above the interval would keep the radio awake; the
     * interval wins and the window is pulled down to it. */
    CHECK(runtime_config_set(&cfg, "wake-window", "900", 240, err, sizeof(err)) &&
              cfg.wake_window_ms == 900,
          "a window below the interval is accepted");
    CHECK(runtime_config_set(&cfg, "wake-interval", "300", 240, err, sizeof(err)) &&
              cfg.wake_interval_ms == 300 && cfg.wake_window_ms == 300,
          "lowering the interval below the window pulls the window down");
    CHECK(!runtime_config_set(&cfg, "wake-interval", "5", 240, err, sizeof(err)),
          "wake-interval below the minimum is rejected");
    CHECK(!runtime_config_set(&cfg, "wake-window", "0", 240, err, sizeof(err)),
          "wake-window 0 is rejected");
    /* clamp() pulls an over-interval window back inside a persisted value. */
    runtime_config_t rs = runtime_config_defaults();
    rs.radio_save = true;
    rs.wake_interval_ms = 100;
    rs.wake_window_ms = 5000;
    runtime_config_clamp(&rs, 240);
    CHECK(rs.wake_window_ms == 100,
          "clamp pulls a persisted window inside the interval");
    rs.wake_interval_ms = 3;
    runtime_config_clamp(&rs, 240);
    CHECK(rs.wake_interval_ms == RUNTIME_CONFIG_WAKE_INTERVAL_MIN_MS,
          "clamp raises a too-small wake interval to the minimum");

    /* clamp(): a value restored from NVS is forced into what this build allows.
     * This is the guard that lets a persisted 'set' survive an OTA into an image
     * with a different ceiling. */
    stored.cpu_mhz = 240;
    stored.cpu_min_mhz = 40;
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

    /* Wi-Fi 6 TWT knobs. Off by default; the wake interval is the negotiated
     * cadence in ms. */
    cfg = runtime_config_defaults();
    CHECK(!cfg.twt_enabled, "twt is off by default");
    CHECK(cfg.twt_wake_ms == RUNTIME_CONFIG_TWT_WAKE_DEFAULT_MS,
          "twt-wake defaults to 5000 ms");
    CHECK(runtime_config_set(&cfg, "twt", "on", 240, err, sizeof(err)) &&
              cfg.twt_enabled,
          "set twt=on");
    CHECK(runtime_config_set(&cfg, "twt-wake", "1000", 240, err, sizeof(err)) &&
              cfg.twt_wake_ms == 1000,
          "set twt-wake=1000 ms");
    CHECK(runtime_config_set(&cfg, "twt", "off", 240, err, sizeof(err)) &&
              !cfg.twt_enabled,
          "set twt=off");
    CHECK(!runtime_config_set(&cfg, "twt", "maybe", 240, err, sizeof(err)),
          "a non-bool twt value is rejected");
    CHECK(!runtime_config_set(&cfg, "twt-wake", "100", 240, err, sizeof(err)),
          "twt-wake below the minimum is rejected");
    CHECK(!runtime_config_set(&cfg, "twt-wake", "20000", 240, err, sizeof(err)),
          "twt-wake above the maximum is rejected");

    /* twt_split: interval = mant * 2^expn [us]. 5 s = 5,000,000 us needs
     * expn 7 (5e6 / 2^7 = 39062.5 -> rounds up to 39063); 1 s = 1,000,000 us
     * needs expn 4 (1e6 / 16 = 62500, exact). */
    uint8_t expn = 0;
    uint16_t mant = 0;
    CHECK(runtime_config_twt_split(5000000u, &expn, &mant) && expn == 7 && mant == 39063,
          "5 s splits to mant 39063 * 2^7");
    CHECK(runtime_config_twt_split(1000000u, &expn, &mant) && expn == 4 && mant == 62500,
          "1 s splits to mant 62500 * 2^4");
    CHECK(!runtime_config_twt_split(0u, &expn, &mant), "a zero interval is rejected");
    /* The mantissa must stay in the 16-bit field and the exponent in 5 bits. */
    CHECK(runtime_config_twt_split(10000u, &expn, &mant) && expn <= 31,
          "a small interval stays in range");
    CHECK((uint32_t)mant * (1u << expn) >= 10000u,
          "the split never shortens the requested interval");

    /* clamp() forces a persisted TWT wake interval into range. */
    runtime_config_t tw = runtime_config_defaults();
    tw.twt_wake_ms = 100;
    runtime_config_clamp(&tw, 240);
    CHECK(tw.twt_wake_ms == RUNTIME_CONFIG_TWT_WAKE_MIN_MS,
          "clamp raises a too-small twt wake interval");
    tw.twt_wake_ms = 60000;
    runtime_config_clamp(&tw, 240);
    CHECK(tw.twt_wake_ms == RUNTIME_CONFIG_TWT_WAKE_MAX_MS,
          "clamp lowers a too-large twt wake interval");

    /* Blob roundtrip: encode then decode restores every knob, including TWT. */
    runtime_config_t rt = runtime_config_defaults();
    rt.cpu_mhz = 160;
    rt.cpu_min_mhz = 80;
    rt.light_sleep = false;
    rt.espnow_retries = 7;
    rt.wifi_txpower_dbm = 8;
    rt.ap_beacon_tu = 200;
    rt.radio_save = true;
    rt.wake_interval_ms = 300;
    rt.wake_window_ms = 250;
    rt.twt_enabled = true;
    rt.twt_wake_ms = 1000;
    uint8_t blob[RUNTIME_CONFIG_BLOB_SIZE];
    runtime_config_encode(&rt, blob, sizeof(blob));
    CHECK(blob[0] == RUNTIME_CONFIG_BLOB_VERSION, "blob carries the current version");
    runtime_config_t back;
    CHECK(runtime_config_decode(blob, sizeof(blob), &back), "blob decodes");
    CHECK(back.cpu_mhz == rt.cpu_mhz && back.cpu_min_mhz == rt.cpu_min_mhz &&
              back.espnow_retries == rt.espnow_retries &&
              back.wifi_txpower_dbm == rt.wifi_txpower_dbm &&
              back.ap_beacon_tu == rt.ap_beacon_tu &&
              back.radio_save == rt.radio_save &&
              back.wake_interval_ms == rt.wake_interval_ms &&
              back.wake_window_ms == rt.wake_window_ms &&
              back.twt_enabled == rt.twt_enabled &&
              back.twt_wake_ms == rt.twt_wake_ms,
          "the blob roundtrip restores every knob");
    /* A wrong version or size is rejected so an old blob is ignored. */
    blob[0] = (uint8_t)(RUNTIME_CONFIG_BLOB_VERSION - 1);
    CHECK(!runtime_config_decode(blob, sizeof(blob), &back),
          "a blob with the wrong version is rejected");
    CHECK(!runtime_config_decode(blob, sizeof(blob) - 1, &back),
          "a blob with the wrong size is rejected");

    /* reset restores the defaults. */
    cfg = runtime_config_reset(&cfg);
    runtime_config_t defaults = runtime_config_defaults();
    CHECK(cfg.cpu_mhz == defaults.cpu_mhz &&
              cfg.cpu_min_mhz == defaults.cpu_min_mhz &&
              cfg.light_sleep == defaults.light_sleep &&
              cfg.twt_enabled == defaults.twt_enabled,
          "reset restores the defaults");

    printf("runtime config: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
