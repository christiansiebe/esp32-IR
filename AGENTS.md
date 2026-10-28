# esp32-IR â€“ repository notes for agents

ESP-IDF firmware for the ESP32-IR project (GitHub `christiansiebe/esp32-IR`).
Two firmware roles are built from one source tree.

## Working in this sandbox (read first)
- **Read `TODO.md` first.** It holds the current work state (open + done) and,
  at the top, the "Arbeitsweise: Chat-Heartbeat" section with the rules below.
  Every follow-up chat must pick up the open items from it.
- The chat runs in a sandbox with a ~20 min inactivity timeout. On idle it is
  reset and any **uncommitted** work is lost. A cron automation `chat-heartbeat`
  (`*/15 * * * *`, ACTIVE) pings the chat to keep it alive - but it protects the
  chat, not the work.
- At the start of a chat, check that `chat-heartbeat` is still ACTIVE (recreate
  it if not), then continue the open `TODO.md` items.
- Commit and push small steps promptly; never leave important work only in
  `/tmp`.

## Roles
- **Sender**: UART SML decoder (9600 baud 8N1 on UART1, RX GPIO16) and ESP-NOW
  value transport. No web server, no WLAN/IP of its own. Its page is hosted by
  the receiver and tunnelled over ESP-NOW (see "Sender site" below).
- **Receiver**: ESP-NOW value transport, SPIFFS power history, and the Wi-Fi
  dashboard (AP `ESP32-IR-Meter` + optional STA to the home WLAN). Advertises
  `esp32-meter.local` via mDNS.
- Role selection is a Kconfig `choice` in `main/Kconfig.projbuild`
  (`CONFIG_ESPNOW_ROLE_SENDER` / `_RECEIVER`).
- **Non-negotiable / must-not-break**: ESP-NOW, OTA, roles.
- **ESP-NOW is a link-layer protocol with no IP** (max 250 B per frame). A
  browser cannot open a page "over ESP-NOW"; the sender therefore stays
  WLAN-free and the receiver hosts + tunnels its page.
- **Partition layout**: the `factory` slot was removed to give both OTA slots
  more room (see `partitions.csv` and "Flash layout" below). Keep `storage` at
  its current offset/size so existing SPIFFS history survives a re-flash.

## Build and configuration
- No committed `sdkconfig` (git-ignored). Configuration comes from
  `sdkconfig.defaults` plus per-role overrides.
- CI sets `SDKCONFIG_DEFAULTS`, which **hides** a project `sdkconfig.defaults`
  unless it is listed explicitly. The workflow therefore uses
  `SDKCONFIG_DEFAULTS=sdkconfig.defaults;<temp overrides>`.
- `SDKCONFIG` is a **CMake cache variable, not an environment variable**.
  `idf.py build` invoked as a CLI ignores an exported `SDKCONFIG`; the effective
  config is written to the project `sdkconfig` file. Assert against that file.
- Build image: `espressif/idf:v6.1`, target `esp32`, flash 4 MB.
- Local builds: `tools/build.sh receiver|sender [--clean]` wraps the IDF
  container (host only needs Docker; works with `docker` or `sudo docker`).
- **Firmware version (2026-10-14).** Root `CMakeLists.txt` resolves the version
  from `$FIRMWARE_VERSION` (set by CI) or `git describe --tags --always --dirty`,
  and writes `main/build_version.h` (git-ignored) as `FIRMWARE_VERSION`.
  `main/version.c` exposes it as `fw_version()`, with `PROJECT_VER` / `"unknown"`
  as fallback so host builds still compile. The string is reported as the JSON
  field `version` in `/api`, `/api/dashboard`, `/api/sender` and the sender-diag
  `counters`, and shown on the dashboard status line (`· FW: …`) and in the
  console/diag views (`fw: …`), so a flashed device is identifiable without
  guessing. CI writes the version to `firmware-version.txt` in the flash bundle,
  into the step summary, and asserts the string is present in the built `.bin`.
  Gotcha: the build runs in the IDF container as root while the checkout is
  owned by the runner user, so `git` refuses with "dubious ownership" and the
  version becomes `unknown`; the workflow therefore sets
  `safe.directory $GITHUB_WORKSPACE` and exports `FIRMWARE_VERSION`. Tag releases
  (`git tag -a vX.Y.Z`) give a clean version instead of a bare SHA.
- **Versioning + commit convention: see `VERSIONING.md`.** One SemVer tag per
  firmware (`v0.1.0` and `v0.1.1` are tagged; the current tip is untagged, so
  `git describe` reports `v0.1.1-<N>-g<sha>[-dirty]`). Before `v0.1.0` the
  version was `unknown`.
  The sender's `fw_version()` also travels in the counters frame
  (`espnow_diag_counters_t.fw_version`, `ESPNOW_DIAG_VERSION` 2) and is shown on
  the receiver's sender page as `sender fw:` — so the other device's build is
  visible without guessing. Commits stay role-scoped: only the sender's or the
  receiver's files, and `feat(both):` when a change touches both (e.g. the wire
  format in `espnow_diag.h`). `main/main.c` carries both roles, so split its diff
  per hunk (`git add -p`) when only one side changed.
- **Diag wire version: never require an exact match.** `is_diag_request` /
  `is_diag_response` accept `ESPNOW_DIAG_VERSION_MIN..ESPNOW_DIAG_VERSION`, and
  every response echoes the request's version. Both devices are usually not
  reflashed together, so a strict `version == ESPNOW_DIAG_VERSION` silently drops
  the peer's packets (2026-10-15: bumping to 2 made an un-reflashed receiver look
  like a dead sender). The header layout is unchanged and the counters payload
  only grew at the tail, so old and new versions interoperate. Bump MAJOR of the
  *firmware* only if the header layout or an existing field's meaning changes.

## Flash layout (4 MB)
| Partition | Offset | Size |
|-----------|--------|------|
| nvs       | 0x9000 | 0x6000 |
| otadata   | 0xf000 | 0x2000 |
| phy_init  | 0x11000 | 0x1000 |
| ota_0     | 0x20000 | 0x180000 (1.5 MB) |
| ota_1     | 0x1a0000 | 0x180000 (1.5 MB) |
| storage (SPIFFS) | 0x320000 | 0xE0000 (896 KB) |

- There is **no `factory` slot**: the app is flashed to `ota_0` (COM) and updated
  OTA between `ota_0`/`ota_1`. The two 1.5 MB slots replace the old 1 MB
  factory+2Ã—1 MB OTA layout, giving each image +512 KB headroom.
- `storage` deliberately keeps its old offset **and** size, so upgrading from
  the factory layout preserves the SPIFFS minute history (data is only lost if
  `storage.bin` is flashed or the partition offset/size changes).
- Flash bundles **never** write `storage`: the CI staging step and the committed
  `Firmware/Receiver` bundle strip the `storage.bin` entry from
  `flasher_args.json`, so a bundle/com flash keeps the history. OTA and app-only
  flashes were already safe. Do not re-add the storage entry.
- **Both roles** have the `storage` SPIFFS partition and run `power_history`
  (the sender persists the minutes it decodes from the UART, the receiver
  persists what it receives), so the per-role build emits its own
  `build/<role>/storage.bin`. A USB full flash should write the (empty)
  `storage.bin` **only on the very first deployment** â€” afterwards leaving it out
  keeps the history. A missing/blank partition is auto-formatted at first boot
  (`format_if_mount_failed = true`), so omitting `storage.bin` is safe.
- The ring buffer holds `POWER_HISTORY_CAPACITY` (29000) minute records; when it
  is full it overwrites the oldest slot (`next_slot = (next_slot + 1) %
  CAPACITY`). The value is bounded by the SPIFFS partition, not chosen freely:
  the on-disk file is `2 * header + CAPACITY * sizeof(record) = 56 + CAPACITY*28`
  bytes and SPIFFS keeps free blocks for wear-levelling GC. 30000 records
  (840056 B) did **not** fit in the 896 KiB `storage` partition, so the buffer
  could never wrap and every write past ~29600 records failed with
  `SPIFFS_ERR_FULL` (recurring dashboard write errors). Before changing the
  capacity, re-check it with `components/spiffs/spiffsgen.py`.
- Record format is versioned (`POWER_HISTORY_VERSION`, currently 2). A version
  bump does not drop the stored history: `power_history_migrate_record()` upgrades
  older records in place, kept versions are `[POWER_HISTORY_OLDEST_SUPPORTED_VERSION,
  POWER_HISTORY_VERSION]`, and the CRC stays strict (a format change is not
  corruption). When bumping a version, add the migration step and a host test.

## CPU clock / power
- The DFS ceiling is **single-sourced** in `CONFIG_ESPNOW_CPU_MAX_MHZ`
  (`main/Kconfig.projbuild`, range 40..240) and is **role-specific**: the
  receiver defaults to **240 MHz** (the dashboard and the minute-history
  backfill are CPU/SPIFFS bound), the sender defaults to **80 MHz**. The sender's
  radio is held awake for a reliable ESP-NOW link (see the power-save note
  below), so the Wi-Fi driver holds `ESP_PM_APB_FREQ_MAX` and DFS can never step
  below the ceiling anyway; a low ceiling is the effective "DFS off, run at
  80 MHz" setting and avoids the heat/power the clock would otherwise never
  leave. The floor stays at the 40 MHz XTAL, so if the APB lock is ever released
  (the opt-in modem-sleep experiment) the clock can still drop to 40 MHz.
  `apply_power_management()` in `main.c` applies the ceiling for **both** roles
  (light sleep off for the receiver, which must stay responsive for HTTP +
  ESP-NOW) and `uart_sleep_window.c` re-applies it with light sleep on for the
  sender. Keep `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` (240, `sdkconfig.defaults`) at
  or above the ceiling; power management lowers the boot clock to it.
  History: the sender was pinned at 80 MHz (commits `40baea3`/`fa24b5e`/
  `c53b028`), then moved to 240 MHz; it is back at 80 MHz now that the radio is
  known to be held awake on this chip.
- The console `set cpu|freq|cpu-min|sleep|retries` knobs are **persisted in NVS**
  (namespace `rtcfg`, blob versioned): a value survives a reboot **and** an OTA,
  so the clock can be pinned from the dashboard without reflashing.
  `runtime_config_load()` reads it in `active_runtime_config()` and clamps it to
  the running build's ceiling, so an image with a lower `CONFIG_ESPNOW_CPU_MAX_MHZ`
  can never restore an out-of-range clock. `reset-config` erases the blob and
  returns to the compile-time defaults. Host-tested (`test/test_runtime_config.c`).
- **Generated `sdkconfig*` files are git-ignored** (only `sdkconfig.defaults` is
  tracked). `sdkconfig`, `sdkconfig.receiver`, `sdkconfig.sender` are local
  artefacts; an old local `sdkconfig.*` pins `CONFIG_ESPNOW_CPU_MAX_MHZ` and
  **overrides** `sdkconfig.defaults`, so after changing the ceiling re-run
  `idf.py reconfigure` (or delete the local `sdkconfig.*`) or the build keeps
  the old value. The CI matrix regenerates `sdkconfig` from `sdkconfig.defaults`
  + role defaults, so CI is unaffected by stale local files.
- The sender reports its live clock and sleep state in the `counters`
  diagnostic section: `cpu_freq_mhz` (from `esp_clk_cpu_freq()`, i.e. the DFS
  clock as it is right now), `cpu_freq_max_mhz`/`cpu_freq_min_mhz` (the
  configured DFS range), `light_sleep_enabled`, `sleep_state` (0 awake /
  1 light sleep) and `uptime_sleep_ms` (accumulated light-sleep time). The
  receiver's own dashboard shows the same CPU clock and sleep state in the
  meter status line, so both devices' power saving is visible.
- Trade-off accepted deliberately: on the ESP32 the Wi-Fi driver holds an
  `ESP_PM_APB_FREQ_MAX` lock while interfaces are up, and light sleep can make
  the receiver miss the sender's connectionless ESP-NOW broadcasts. That is
  **safe** because of the history backfill (below): any minute missed over the
  air is pulled from the sender's own SPIFFS history afterwards. If ESP-NOW ever
  proves unreliable on the bench, drop `CONFIG_PM_ENABLE` to get the old fixed
  behaviour; the CI job "Verify the variable CPU clock is in effect" checks the
  four PM options and the role-specific ceiling (sender 80, receiver 240).
- Wi-Fi power save stays at the default `WIFI_PS_MIN_MODEM`; `WIFI_PS_NONE`
  would burn power and power save beyond the default hurts ESP-NOW reliability.
- **ESP-NOW connectionless power save (`CONFIG_ESPNOW_SENDER_ESPNOW_POWER_SAVE`,
  default OFF).** The sender's station is never associated with an AP, so the
  Wi-Fi driver keeps `ESP_PM_APB_FREQ_MAX` asserted and the radio on all the
  time; that pins DFS at the ceiling and blocks light sleep entirely ("240 MHz,
  0 steps, slept 0.0 s"). `WIFI_PS_MIN_MODEM` only helps a *connected* station.
  The documented fix is a periodic wake window on the connectionless module:
  `esp_wifi_connectionless_module_set_wake_interval()` +
  `esp_now_set_wake_window()` with window < interval
  (`CONFIG_ESPNOW_SENDER_ESPNOW_WAKE_{INTERVAL,WINDOW}_MS`, default 100/70 ms),
  set in `uart_sleep_init_idf()` after `esp_wifi_start()`; the driver then drops
  the radio (and the APB lock) between windows. **It is opt-in and off by
  default because on the original ESP32 it was observed to break the ESP-NOW
  link ("sender connects no more") even though the API is documented as
  supported.** With it off the sender keeps the Wi-Fi radio on, i.e. exactly the
  behaviour before the option existed. Do not turn it on without a bench test
  that the link survives. When on, a request arriving while the radio is off is
  retried: `sender_diag_exchange_raw()` retries idempotent requests up to
  `SENDER_DIAG_ATTEMPTS` (4); a command uses the single-shot
  `sender_diag_exchange_once()` so it cannot run twice. See Espressif issue
  esp-idf#11714 ("ESP-NOW does not release the WiFi module and APB_FREQ_MAX").
  **Two things are needed together, and both are opt-in
  (`CONFIG_ESPNOW_SENDER_ESPNOW_POWER_SAVE`, default off):**
  1. `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` **after `esp_wifi_start()`**. The old
     call lived in `uart_sleep_init_idf()`, which runs *before* `esp_wifi_init()`,
     so it returned `ESP_ERR_WIFI_NOT_INIT` and was silently ignored — the
     driver then holds `ESP_PM_APB_FREQ_MAX` forever and DFS is pinned.
  2. `esp_now_set_wake_window()` + `esp_wifi_connectionless_module_set_wake_interval()`
     (window first, then interval), applied from `esp_now_init()` and re-applied
     from the sender's `WIFI_EVENT_STA_CONNECTED` handler.
  **Verified on hardware (ESP32 classic, IDF v6.1):** with both on, the API
  accepts the calls (`err=0`) and the radio does cycle — but the ESP-NOW link
  then breaks (the link watchdog fires: `sleep_fallback:1`, light sleep off). So
  the radio-cycling power save and a reliable ESP-NOW link are **not both
  available on the ESP32 classic**. Since "no data may be lost" wins, the option
  is **off by default**: the radio stays on and DFS stays pinned at the ceiling
  (0 steps). This is a deliberate trade-off, not a bug.
  `CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y` (in `sdkconfig.defaults`) is
  required for the wake window to apply without an association, should the
  option be turned on for experiments.
- **`sleep_fallback:1` right after a reboot is normally benign.** The receiver
  only talks to the sender on demand (dashboard / OTA) or every 30 min (history
  sync); the sender's own link watchdog (`link_fallback`, `ESPNOW_LINK_FALLBACK_MS`
  = 60 s + `ESPNOW_LINK_FALLBACK_GRACE_MS` = 30 s ≈ 90 s after boot) latches when
  nothing arrives in that window. With the radio held awake the sender still
  *receives*, so this only drops light sleep — which the default image no longer
  relies on for the link anyway. It is sticky until the next boot, so a reboot
  that no poll follows will always show `sleep_fallback:1`; that is not a fault.
  Verify health with the *live* signals instead: `last_rx_ago_ms` stays low,
  `sml_send_fail` is low, the receiver's `sender_rssi_valid` is true and the
  history sync reports `caught_up: true` / `errors: 0`.
- **Sender association is optional and OFF by default
  (`CONFIG_ESPNOW_SENDER_ASSOCIATE`, `ESPNOW_SENDER_ASSOC_{SSID,PASSWORD}`).** A
  bench test showed that associating the sender with the receiver SoftAP (station
  reports `associated: connected`) does **not** by itself make DFS step down.
  Association is kept as an experiment only. When on, the sender re-associates on
  `STA_DISCONNECTED`; the ESP-NOW broadcast to the receiver AP's BSSID works
  either way.
- **Relayed sender images are validated before activation.** The sender has no
  USB and is reachable only over ESP-NOW, so a bad image would be unrecoverable
  (its `boot_confirm.c` is a no-op stub). `relay_sender_firmware()` reads the
  freshly written partition's `esp_app_desc_t` (offset 0x20) and refuses to
  `esp_ota_set_boot_partition()` it unless `project_name == "esp32-IR"`, pointing
  the boot partition back at the running image on failure.
- **Receiver without the SoftAP (`CONFIG_ESPNOW_RECEIVER_DISABLE_AP`, default
  off).** Station-only mode: no `ESP32-IR-Meter` AP, dashboard over the home
  WLAN only (DHCP/mDNS). ESP-NOW runs on `ESPNOW_IF` (station when the AP is off,
  otherwise the AP interface) for every receiver-side peer. Because the sender
  learns the channel by scanning for that AP, the AP-off receiver requires both
  roles built with the same `CONFIG_ESPNOW_CHANNEL` (now configurable); the
  sender falls back to it when the scan finds nothing, and a station-connected
  receiver cannot leave its home AP's channel, so pick that channel.
- **Do NOT drop the SoftAP at runtime while ESP-NOW is on the AP interface.**
  The receiver's ESP-NOW peers use `ESPNOW_IF = WIFI_IF_AP` in the default
  APSTA build (see `2ec4d36`), so calling `esp_wifi_set_mode(WIFI_MODE_STA)` to
  save power tears the AP interface down and silently kills the whole ESP-NOW
  link (diag, history backfill, OTA target, broadcast). Symptom: after
  `STA_GOT_IP`, `/api/rssi` reports `active:false`, `/api/sender-diag` answers
  "Sender did not answer", and `/api/history-pull` shows `errors` climbing.
  A runtime "AP off once the station is up" feature was tried (`04add16`) and
  reverted (`71409c8`) for exactly this reason. The SoftAP stays on; only the
  compile-time `CONFIG_ESPNOW_RECEIVER_DISABLE_AP` path (which uses
  `WIFI_IF_STA` and never has an AP to drop) is safe.
  **Deployed and verified (2026-10-17, `feat/receiver-ap-off`, PR #13):** the
  CI matrix now builds a `receiver-apoff` image; flashed to the receiver
  (`db228f9`) the dashboard came up over the WLAN (`192.168.178.70`, `/healthz`
  200), the `ESP32-IR-Meter` SoftAP was gone and the ESP-NOW link stayed alive
  (`history-pull`: `caught_up: true`, `errors: 0`, `sender_mac_known: true`;
  sender round-trip via `/api/sender-diag` ok). The home AP "Wohnzimmer" runs on
  channel 1, which is why `CONFIG_ESPNOW_CHANNEL=1` still matches.
- **Fixed (PR #14): the receiver's `web_guard` restarted the HTTP server while
  idle — unsigned underflow in `web_guard_tick()`.** The guard captured `now_ms`
  *before* the self-probe, but the `/healthz` handler stamps `last_request_ms`
  *during* it, a few ms later. So `last` (e.g. 100183) exceeded `now` (100169)
  and `now_ms - last` wrapped on `uint64_t` to a near-2^64 value that reads as an
  endless silence (> the 30 s threshold) → a **false restart every ~45 s** even
  though the dashboard was reachable from outside. Hardware proof:
  `tick=RESTART last=100183 now=100169 thr=30000 probe_err=0(ESP_OK) status=200`.
  The fix clamps the subtraction (`silence = now_ms >= last ? now_ms - last : 0`)
  and a host test (`test_probe_timestamp_ahead_of_now`) now covers the
  future-stamp case. Verified on device (`428fdaf`): `restarts 0`, `failures 0`
  over 240 s (was ~5 restarts/window). The occasional `ESP_ERR_HTTP_EAGAIN` probe
  error was a red herring (about once per 2 min, never ≥30 s in a row). Kept the
  diagnostics that found it: a `self-probe failed: <err>` WARN and a
  `web probe: last err …, http …, failures …` line in the `status` command.
- Adaptive sender light sleep (`CONFIG_ESPNOW_SENDER_LIGHT_SLEEP`, default
  **on**, sender ships as a single image): `main/uart_sleep_window.{h,c}` parks
  the CPU only in the quiet gap between SML telegram bursts. The window starts at
  400 ms, is capped below the shortest inter-telegram gap seen (3/4 margin),
  grows 25 ms per clean sleep and halves on an overrun. An
  `ESP_PM_NO_LIGHT_SLEEP` lock is held for the whole burst so the APB clock never
  stops mid-telegram (there is no UART wake-up source). `uart_sleep_hold()` from
  the ESP-NOW receive path keeps the CPU awake for an in-flight history pull (2 s)
  or OTA (5 s). Enabling it also sets `WIFI_PS_MIN_MODEM`, which is what lets the
  Wi-Fi driver release its `ESP_PM_APB_FREQ_MAX` lock so light sleep is entered at
  all. The `counters` diagnostic reports `build_profile` (1 = this image) so the
  running image is unambiguous. Host tests: `test/test_uart_sleep_window.c` and
  `test/test_uart_sleep_sim.c` (the sim models the lost bytes when a telegram
  arrives mid-sleep). CI syntax-checks the enabled branch with IDF stubs in
  `test/host/include` and greps the built sender image for the sleep log string.
  Bench verification of the enabled path is still open (see `TODO.md`).
- Gotcha (fixed): the sleep code is guarded by
  `#if CONFIG_ESPNOW_SENDER_LIGHT_SLEEP && SOC_LIGHT_SLEEP_SUPPORTED`. It must NOT
  be guarded on `SOC_PM_SUPPORT_LP_TIMER`/`SOC_PM_SUPPORT_EXT_WAKEUP`: the ESP32
  `soc_caps.h` does not define `SOC_PM_SUPPORT_LP_TIMER`, so such a guard is
  silently false and the whole file compiles to the no-op stub — the device then
  reports `build_profile: sender-sleep` together with `window: 0 ms` and
  `light sleep disabled`. Any capability macro used in a `#if` guard must be
  confirmed to exist for the target. The CI grep must match a string that lives
  in the real branch only (e.g. `adaptive sleep path compiled in`), never one in
  `main.c`, or it passes even when the stub is compiled.

## Tests
- Host tests: `./test/run_host_tests.sh` plus
  `python3 test/test_ha_stats.py`, `test/test_ws_min.py`,
  `test/test_ws_client.py`, `test/check_dashboard_js.py`.
- `test/check_dashboard_js.py` extracts the dashboard JS embedded in a C string
  in `main/main.c` and checks syntax and a runtime smoke test with Node
  (Node is a test-time dependency only). It also syntax-checks the diagnostics
  page JS from `diag_page_handler` in `main.c`.
- `test/test_sml_parser.cpp` exercises the vendored SML parser under
  AddressSanitizer/UBSan (depth guard in `smlNewList`, OBIS bounds on empty and
  truncated lists, fuzzed byte streams). It is built and run by
  `run_host_tests.sh`, so a parser buffer overrun fails CI.
- `test/stability/run_stability.sh` (also run by `run_host_tests.sh`) builds the
  power-history logic under AddressSanitizer/UBSan and drives it with ten
  fault-injection tests (bit flips, random garbage, version/boundary matrix,
  corrupt-record mixes, argument guards). It must never crash or accept an
  inconsistent record.

## Crash diagnostics (sender)
- The sender persists a small, CRC32-checksummed crash journal in NVS
  (`crash_journal.c` + `crash_journal_store.c`): counters plus the tail of the
  diagnostic event ring. On boot `crash_journal_init()` logs the reset reason
  and whether the previous session ended cleanly (shutdown handler marks clean
  reboots). The receiver reads it via the `crash` diagnostic section
  (`/api/sender-diag?section=crash`, shown on `/diag`).
- Writes are rate limited to protect NVS endurance: ERROR events at most every
  5 s, INFO/WARN at most every 60 s, plus a 60 s heartbeat in the UART stats
  task. The wire response is a single ESP-NOW payload (243 bytes, guarded by a
  `_Static_assert` in `main.c`).

## UART receive-gap diagnosis (sender)
- The read head (IR diode UART) delivers bytes in bursts; silent gaps between
  telegrams are normal. `uart_capture_worker` measures the gap between byte
  bursts (threshold `UART_GAP_THRESHOLD_MS`, 50 ms) and classifies each gap with
  `sml_adapter_awaiting_telegram()`: `SML_START` (waiting for a telegram) or
  `SML_FINAL` (previous telegram just delivered) = between telegrams (fine),
  anything else = a gap inside a telegram (bytes lost, the parser will fail its
  checksum). Counters: `gap_count`/`gap_quiet`/`gap_mid`/`gap_loss`/
  `last_gap_ms`/`max_gap_ms`, plus `last_rx_ago_ms`. A mid-telegram gap is not
  proof of loss by itself: only a following failed checksum increments
  `gap_loss` (the meter may pause inside a split telegram and still pass).
  A total silence longer than `UART_SILENCE_ALERT_MS` (30 s) logs a one-shot
  ERROR event.
- Parser-stuck recovery: if the parser is still inside a telegram
  (`!sml_adapter_awaiting_telegram()`) and no byte has arrived for
  `UART_PARSER_STUCK_MS` (6 s), the frame is truncated. `uart_capture_worker`
  flushes the UART input and calls `sml_adapter_reset()` so the next preamble
  resynchronises it, counts `parser_resyncs`, and logs a WARN. This keeps the
  sender self-healing when the read head loses the byte stream instead of
  staying desynchronised (which looks like a hang).
- These are exposed in the counters diagnostic section and rendered with a
  plain-language verdict on `/diag`.

## Sender wall clock (time sync over ESP-NOW)

Only the receiver has an NTP-synced clock (`esp_netif_sntp`, started on
`IP_EVENT_STA_GOT_IP`); the sender has no network and only `esp_timer` uptime.
The sender therefore asks the receiver for the time (section flag
`ESPNOW_DIAG_SECTION_TIME_SYNC`, `espnow_diag_time_sync_t`) once on boot and
then hourly. The request carries the sender uptime; the response carries the
receiver's `time(NULL)` seconds and a validity flag (`system_time_is_valid`).
The sender stores `offset = utc - uptime` and stamps `diag_log_event` with real
time (`event_wall_ms`), falling back to uptime until the first sync. Between
calibrations the sender counts on its own crystal (10-50 ppm, so under ~0.2 s
per hour). The receiver MAC is learned from the time-sync response and from
incoming diagnostic requests; until it is known the request is broadcast. The
`/diag` dashboard shows absolute ISO time once stamps look like real dates
(> 1e12), otherwise relative `T-xx s` or raw uptime, and lists `time_syncs`
(successful calibrations) next to `time_requests` (requests sent). If
`time_requests` grows but `time_syncs` stays 0, the receiver has no NTP fix
(check `clock_synced` on the receiver) or still runs old firmware.

## Sender site (receiver-hosted, tunnelled over ESP-NOW)
- ESP-NOW is not IP, so the sender has no addressable page. The receiver hosts
  the sender UI and tunnels every request over the existing diagnostic channel
  (`sender_diag_exchange` / `sender_cmd_exchange` in `main.c`), exactly like
  `/api/sender-diag`.
- Pages/endpoints (receiver only, all guarded by `#if !CONFIG_ESPNOW_ROLE_SENDER`):
  - `GET /sender` (`sender_page_handler`): landing page with status, previous
    session / last crash, recent events, sender->receiver RSSI and the console.
  - `GET /api/sender` (`sender_status_api_handler`): one JSON object built from a
    counters pull plus a best-effort crash pull; events are paged separately via
    `/api/sender-diag?section=events`.
  - `POST /api/sender-console` (`sender_console_api_handler`, `X-OTA-Key`): sends
    one command line to the sender; HTTP 422 when the command is unknown.
- Command channel: new section flag `ESPNOW_DIAG_SECTION_CMD` with wire structs
  `espnow_diag_cmd_request_t` / `espnow_diag_cmd_response_t` (declared in
  `main.c`, both `_Static_assert`ed to fit one ESP-NOW payload). The sender runs
  the line through the same `web_console_run()` used by its local console
  (`status`, `time`, `wifi`, `reboot`, ...) and returns the captured text
  (truncated to `ESPNOW_DIAG_CMD_OUTPUT_LEN`).
- Sender->receiver RSSI: the receiver stores the last `info->rx_ctrl->rssi` from
  an SML packet in `g_sender_rssi_dbm` and reports it as `sender_rssi`. The
  sender cannot measure this direction itself (it only transmits).
- mDNS (`espressif/mdns` in `main/idf_component.yml`): `start_mdns()` sets
  `esp32-meter.local` and advertises `_http._tcp`, started on `WIFI_EVENT_AP_START`
  and `IP_EVENT_STA_GOT_IP` (idempotent). Only the receiver advertises a name.

## OTA from the sandbox (pull OTA, no LAN route needed)
- The sandbox has **no route into the LAN** (DS-Lite); it can never open a
  connection to the device. But **port 12000 is served publicly** as
  `https://work-1-<RUNTIME_ID>.prod-runtime.all-hands.dev/` (Let's Encrypt), and
  the device *can* pull from there. That is the whole trick.
- Firmware side: `POST /api/ota-pull` (JSON `{"url":"https://..."}` or a raw URL,
  header `X-OTA-Key` = Wi-Fi/AP key). It runs in its own task (a TLS handshake
  needs more stack than the HTTP task has), accepts **https only**
  (`ota_url_is_trusted`), refuses a target that would overlap the `storage`
  measurement partition (`ota_target_is_safe`), validates the image
  (`esp_https_ota_get_img_desc` / `esp_https_ota_finish`) and reboots to apply.
  The `/update` page also has a URL field that calls it.
- Sandbox side: `tools/ota-publish.sh <bin> [device-ip]` copies the built image
  into `/workspace/uploads/` (served at `/files/<name>`), starts the upload
  server if needed, verifies the public URL returns HTTP 200, and prints the
  exact `curl` that triggers the update.
- To actually reach the device, run that `curl` on a LAN host. With the Odroid
  command channel up (`remote-shell/cmd_send.sh`), the sandbox can drive it:
  `bash /workspace/remote-shell/cmd_send.sh "<printed curl>"`.
- `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (sdkconfig.defaults) is required: the
  front-door cert is Let's Encrypt, so the public CA bundle is the trust path.
- ESP-NOW path unchanged: `/update-sender` still relays to the sender.

## Post-OTA rollback confirmation (receiver)
- A freshly flashed image starts in `ESP_OTA_IMG_PENDING_VERIFY`
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`). Until it is confirmed, any reset
  rolls back to the previous slot.
- `boot_policy.c` holds the pure decision (`boot_evaluate`, `boot_action_effect`,
  host-tested) and `boot_confirm.c` is the glue: confirm after 45 s up with the
  dashboard reachable, roll back if the dashboard is still down after the 30 s
  grace window. `boot_confirm_poll()` maps the decided action through
  `boot_action_effect()` to the effect, so a failure scenario actually performs
  the rollback (previously `BOOT_TAKE_ROLLBACK` was computed but dropped, and
  `boot_confirm_fail()` had no caller).
- Failure cases are covered twice on the host: `test/test_boot_policy.c` (the
  decision) and `test/test_boot_confirm_trigger.c` (the real glue performs the
  rollback exactly once, and a confirmed image only reboots, never rolls back).
- Health failures after confirmation cannot roll back (that slot is gone); they
  only reboot.

## Receiver health watchdog (self-heal reboot, receiver only)
- The receiver's failure mode is not a crash but a silent hang: the SML data
  stops arriving (sender or ESP-NOW lost) while the device keeps serving a stale
  dashboard. `health_task.c` (started from the receiver branch of `app_main`,
  receiver-only) evaluates the pure rules in `health_policy.{h,c}` every 5 s and
  reboots when the meter data has been silent for longer than
  `CONFIG_ESPNOW_HEALTH_STALE_DATA_S` (default 900 s) or the free heap dropped
  below `CONFIG_ESPNOW_HEALTH_LOW_HEAP_BYTES` (12 KB), after a
  `CONFIG_ESPNOW_HEALTH_GRACE_S` (180 s) start-up window. A reason **latches**,
  so the reboot decision does not flap.
- `last_rx_ms == 0` (no value received yet, e.g. no sender in range during
  commissioning) is deliberately **not** treated as stale, so a receiver without
  a sender does not reboot-loop. `health_task_note_rx()` is called from
  `store_sml_value()` for every received value (any OBIS).
- The reboot is escalated through `boot_confirm_fail()`: a still-unconfirmed
  image rolls back to the previous OTA slot, a confirmed one just restarts. This
  is the post-confirmation health path the boot policy cannot cover (that slot
  is gone).
- It complements `web_guard.c` (dashboard dead) and `liveness_guard.c` (global
  stall): the health watchdog covers "the UI answers but the measurement data
  never arrives". The health worker also subscribes to the TWDT
  (`TASK_WDT_NAME_HEALTH`, receiver-only in `task_wdt_policy.c`), so a wedged
  health loop reboots too. The decision is pure and host-tested
  (`test/test_health_policy.c`); the glue is compiled only for the receiver
  (the sender build has no `health_*` object). Enable/disable with
  `CONFIG_ESPNOW_RECEIVER_HEALTH_WATCHDOG` (default on).

## Watchdog (task watchdog subscribed; idle-task check still off)
- The long-running workers are registered with the task watchdog (TWDT) behind
  `main/task_wdt_policy.{h,c}` (pure, host-tested) + `main/task_wdt_guard.{h,c}`
  (the IDF glue). `task_wdt_guard_init()` runs early in `app_main` and adopts the
  running watchdog (`esp_task_wdt_reconfigure`) with a **generous timeout (120 s)
  and `trigger_panic = true`**, so a wedged worker reboots instead of hanging.
  `power_history_worker` (both roles) and the sender's `uart_event_worker` /
  `uart_capture_worker` call `task_wdt_guard_subscribe()` on entry and
  `task_wdt_guard_feed()` once per loop; the receiver's health worker
  (`TASK_WDT_NAME_HEALTH`) does the same. The IDF default is 5 s / no panic; the
  policy overrides it at runtime, so the `sdkconfig` still shows the default.
  The `status` console command prints the live state.
- Why the timeout is 120 s: the history writer drains the queue in batches and
  may legitimately wait up to `POWER_HISTORY_BATCH_TIMEOUT_MS` (60 s) for more
  records before it writes. That wait is now **bounded** (was `portMAX_DELAY`) and
  the writer feeds the watchdog around it, so an idle-but-healthy writer never
  looks stalled, while a truly wedged one (mutex deadlock, a SPIFFS call that
  never returns) is caught within 120 s. `task_wdt_worker_max_block_ms()` is the
  single source for each worker's longest legitimate block, and
  `task_wdt_policy_covers_role()` proves the timeout covers every subscribed
  worker with margin. Host test: `test/test_task_wdt_policy.c` (119 checks).
- `CONFIG_ESP_TASK_WDT_INIT=y` (IDF default) still starts the watchdog, so the
  idle tasks stay monitored; `task_wdt_guard_init()` mirrors
  `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1` into `idle_core_mask` so
  reconfiguring does not change which idle cores are watched. So a hang in a
  watched task already reboots the device and shows up as
  `task-watchdog`/`interrupt-watchdog` in the crash journal.
- The sender's workers all block on a queue/semaphore/UART read with a finite
  timeout and have no busy loop, so they should not trip the TWDT. `app_main`
  returns after `run_sender()`, and the IDF main task is subscribed to the TWDT
  by default; it does not idle-panic here (default `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1`).
- Consideration for a future change: enabling `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0`
  would catch a fully starved CPU0, but on the ESP32 the Arduino-ESP32 core
  disables the idle-task checks specifically to avoid false resets, and the
  project has no hardware-in-the-loop test to validate it. Left off deliberately.

## Persistence runs on BOTH roles
- `power_history_init()`, the 1 s `g_power_sample_timer` and the history worker
  are started in `app_main` *outside* the `CONFIG_ESPNOW_ROLE_SENDER` branches,
  so sender and receiver both keep a local SPIFFS minute history. Feed points
  differ: the receiver records received telegrams, the sender records what it
  decodes off UART (`record_sender_power`). Do not move that init into a role
  branch without keeping the other role covered.
- The store dedupes by minute: `write_records()` skips a record whose
  `interval_start_utc` is already on disk (`minute_is_stored()`, a binary search
  over the time-ordered ring), so the live recorder and the backfill sync cannot
  create the same minute twice. `power_history_has_minute()` exposes the same
  check to the sync's per-minute dedupe.
- This is why managed components are declared globally in `main/idf_component.yml`
  (mqtt + cjson): the component manager resolves them for both roles, but
  `mqtt_bridge.c` is only compiled for the receiver and `tx_batch.c` only for
  the sender (`main/CMakeLists.txt`).

## Sender ESP-NOW batching (5 s cycle)
- The sender no longer sends one packet per telegram. `sml_adapter_telegram_callback`
  pushes into `g_sml_tx_batch` (`main/tx_batch.c`, host-tested) and
  `uart_transport_worker` waits `SML_TX_CYCLE_MS` (5000) then `flush_tx_batch()`
  drains up to `SML_TX_BURST` (32) oldest-first. The sequence is the ring's own.
- **Unconfirmed sends are retried, not dropped.** `send_sml_telegram()` returns
  true only when the ESP-NOW callback reported `ESP_NOW_SEND_SUCCESS`; otherwise
  `flush_tx_batch()` puts the item back at the head with
  `sml_tx_batch_requeue_front()` and stops the phase, so the telegram is sent
  again next cycle. `sml_tx_batch_mark_sent()` therefore advances only on
  confirmed delivery, and `sml_tx_batch_missing()` counts sequences not yet
  confirmed (0 in normal operation). The ring is `SML_TX_BATCH_CAPACITY` (256),
  far more than one cycle, so a normal cycle never evicts; a long stall (>256
  unconfirmed at ~1 telegram/s â‰ˆ 4â€“5 min) evicts the oldest and that value shows
  up in `batch_missing`/`tx_queue_drops` on the sender diag page.
- Light sleep between cycles: see "CPU clock / power" above. The radio stays on
  by default (`WIFI_PS_MIN_MODEM`).
- **Note**: the SML telemetry is sent by *broadcast*, and on the ESP32 the send
  callback for broadcast reports the driver hand-off, not a real MAC ACK. The
  retry above therefore only bites for genuine driver errors. A true
  delivery-confirmed path would need unicast (as the diagnostics already use).

## History backfill (receiver pulls the sender's minutes)
- Both roles persist their own minute history (`power_history.c`). When the
  receiver misses minutes (radio gap, sleep, a restart of either side) it asks
  the sender for them over ESP-NOW, section `ESPNOW_DIAG_SECTION_HISTORY`
  (`0x0010`), and merges them into its own store. This is the data-loss safety
  net that makes the power-management trade-off above acceptable.
- Protocol: request = header + `from_utc`/`to_utc` + `after_sequence` cursor
  (`espnow_diag_history_request_t`); response = header + up to
  `ESPNOW_DIAG_HISTORY_RECORDS_PER_PACKET` (7) `power_history_record_t` + a
  `MORE` flag (`espnow_diag_history_response_t`). Both structs live in `main.c`
  (they embed `power_history_record_t`) with a `_Static_assert` against
  `ESP_NOW_MAX_DATA_LEN`.
- Sender side: `build_diag_history_response()` -> `power_history_read_window()`
  (binary-search to `from_utc`, block-read forward, stop at `to_utc`, `*more`
  when a further in-window record exists).
- Receiver side: `history_backfill_worker()` runs a session ~15 s after boot and
  every 30 min; a session marks the minutes it already holds into a presence map
  (`main/history_backfill.c`, host-tested: window/dedupe logic), then pages the
  sender for the holes and calls `power_history_merge()` (validates/migrates and
  enqueues; the pull loop already filtered duplicates). The window is the last
  `HISTORY_BACKFILL_WINDOW_SECONDS` (3 days); a deeper gap needs a manual pull.
- Manual/tools: `POST /api/history-pull` (X-OTA-Key) triggers a session,
  `GET /api/history-pull` shows `merged_total`/`pages_total`/`errors`, and the
  receiver console command `pull` runs one synchronously ("N record(s) merged").
  The dashboard also has a "Datenbank jetzt aktualisieren" button that
  `POST /api/db-refresh` (no key, local action) → `history_backfill_request_full()`,
  i.e. it forces a full re-mirror now instead of waiting for the periodic session.
- CSV restore (dashboard "CSV hochladen", `POST /api/power-history-upload`, no key):
  re-imports a backup downloaded from `/api/power-history.csv`. The body is streamed
  line by line (a full store is ~29000 rows and does not fit in the heap), parsed by
  the pure `main/history_csv_import.c` (host-tested), sorted per 64-row batch and
  appended through `power_history_import_sorted()`. That store entry point dedupes by
  minute and, on a full ring, drops the *incoming* record rather than evicting a live
  minute, so a restore can never overwrite the receiver's own data. `sequence` in the
  CSV is ignored (the store renumbers); `reason` is optional and defaults to `none`.
- Sync diagnostics: `GET /api/history-pull` also reports `sessions` (how often a
  session ran), `last_merged`/`last_pages`/`last_duration_ms`/`last_round_ok`
  (what the most recent session achieved and how long it took) and
  `lag_seconds` (how far the resume `cursor_utc` trails the sender's newest
  minute, i.e. the current backfill lag). The receiver dashboard shows this in a
  "Sender-Abgleich" line under the history chart, so a stalled or failing sync
  is visible without reading the console. A healthy install shows
  `caught_up=true`, a small `lag_seconds` and `errors` staying flat; a growing
  lag with `last_round_ok=false` points at the ESP-NOW link or a missing sender
  MAC, while `errors` climbing points at transport timeouts.
- Never overwrite the receiver's own live values: only minutes that are still
  absent in the window are requested and merged, and the store renumbers pulled
  records, so the two independent sequence counters never have to line up.

## Gap semantics (drei verschiedene "Gaps", nicht verwechseln)
1. Sender UART `rx gap` (Sender-Diag): Stille auf der UART-Leitung >= 50 ms.
   Wird per Ursache attribuiert (`gap_cause_{meter,sleep,stall,buffer}`). Ein
   `mid-telegram`-Gap ist nur dann Verlust, wenn das Telegramm danach die
   Prüfsumme verliert (`gap_loss`); der Zähler-Pause-Gap ist normal und tritt
   **auch ohne CPU-Sleep** auf.
2. Empfänger `transport gaps` (ESP-NOW, `g_sml_transport_gaps`): Sequenzsprung
   pro Paket. Die Sequenz wird im Sender **pro Paket** vergeben und ein noch
   nicht gesendetes Paket kann per `sml_tx_batch_requeue_front` mit **alter**
   Sequenz erneut vorne landen (Retry). Daher: Rückwärts/Duplikat
   (`sequence <= last`) = `transport_reorders` (rückgewonnen, **kein** Verlust,
   **kein** Reset der 60-s-Sammlung); Vorwärtssprung (`sequence > last+1`) =
   `transport_losses` (echtes über die Luft verlorenes Broadcast-Paket, kein
   ACK) und nur dann wird die 60-s-Leistungssammlung zurückgesetzt. Diese Gaps
   existieren **auch ohne CPU-Sleep**; der 5-s-Burst (`SML_TX_CYCLE_MS 5000`,
   `SML_TX_BURST 32`) erhöht nur die Verlustrate (~0,65 %), ist aber nicht die
   Ursache der Zählung.
3. Dashboard `overflow` (`/api/sml`, `SML_VALUE_HISTORY_COUNT`): reines
   **Anzeige-Artefakt** — der Browser-Cursor liegt mehr als einen RAM-Ring
   zurück. Kein Messverlust. Der Ring ist jetzt 256 groß; die Meldung heißt
   „DISPLAY GAP", nicht mehr „VALUE GAP".

## Flash budget / lean config
- The 4 MB flash is ~98 % allocated (2×1.5 MB OTA + 896 KB SPIFFS) and the
  receiver image used to fill 84.6 % of its 1.5 MB slot, so unused IDF defaults
  that still get linked are real cost. `sdkconfig.defaults` ("Flash diet")
  disables IPv6 and the WPA3/SAE/OWE/Enterprise Wi-Fi family: measured **−78 KB**
  on the receiver (84.6 % → 79.4 %) and **−90 KB** on the sender. Neither feature
  is used (no IPv6 code; SoftAP is `WIFI_AUTH_WPA2_PSK`; ESP-NOW is a separate
  CCMP link layer, not SAE).
- **Do not disable `CONFIG_ESP_WIFI_MBEDTLS_CRYPTO`**: WPA2 needs it. The OWE
  options `select` it, so turning the WPA3 family off would drop it - that is why
  it is pinned `=y` in `sdkconfig.defaults`. Keep
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` too (pull OTA trusts the public CA).
- Delta OTA (`espressif/esp_delta_ota`, 7.3 KB flash, 0 B static RAM) builds
  cleanly on IDF v6.1 and shrinks an update from 1.25 MB to ~88 KB (measured,
  no compression beats heatshrink). It saves **transfer, not flash**: both OTA
  slots still hold a full image. Not integrated yet - see TODO.md.
- **The app was built with `-Og`, not `-Os`.** IDF's default
  `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` was never overridden, so the receiver
  image was 86 KB (sender 55 KB) larger than necessary. This is now fixed in
  `sdkconfig.defaults` (`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`), together with the
  CMN certificate bundle and the mDNS/MQTT trims - all config, no source change.
  Combined, the receiver went 1251280 -> 1095424 B (-152 KB, 79.4 % -> 69.6 %)
  and the sender 851056 -> 792624 B (-57 KB). Rebuild with
  `tools/build.sh <role> --clean`: an existing `sdkconfig.<role>` keeps the old
  values, SDKCONFIG_DEFAULTS only seeds a fresh config. Trade-off: optimised code
  is harder to single-step; flip back to DEBUG when chasing a bench bug.
- The full CA bundle cost 68 KB (`..._CERTIFICATE_BUNDLE_DEFAULT_FULL=y`, 145
  roots). The CMN bundle is 41 roots / 16.7 KB (measured in the ELF) and still
  has both Let's Encrypt roots (ISRG X1/X2), which is all `main/ota_policy.c`
  (any `https://`) and the sandbox frontend need. Assertion level 2->1 (~12 KB)
  and `ESP_ERR_TO_NAME_LOOKUP=n` (~2 KB) were deliberately left alone - the
  assert text is worth more than 14 KB on a device without USB.
- **Two map traps:** a library listed only in the `LOAD` preamble of
  `esp32-IR.map` (e.g. `libmesh.a`) is a *link candidate*, not linked - check
  `idf.py size-components` instead. And a huge `.rodata.*.str1.4` entry in the
  map is the merged string pool, not one object's data.

## Managed components (IDF v6)
- ESP-IDF v6 moved ESP-MQTT and cJSON out of the core. They come from the ESP
  Component Registry (`espressif/mqtt`, `espressif/cjson`) via
  `main/idf_component.yml`; their component names in the build are
  `espressif__mqtt` / `espressif__cjson`, and the manifest makes them a
  dependency of `main` automatically - naming them in `REQUIRES` is not needed.
- The IDF build compiles with `-Werror`: unused vars and `-Wformat-truncation`
  (e.g. a `snprintf` whose buffer is smaller than the topic could be) fail CI.
  Size buffers for the full topic length.

## Web-Reachability + "kein Datenverlust" (2026-10-14, Branch cline/auftrag-history-dubletten)
Ausgangspunkt war der Report "Verlauf nicht verfügbar: `AbortError`" nach
Sync/CSV-Download. Der `AbortError` ist **client-seitig** (`fetchT`-Helper,
`main.c`, 15 s AbortController), aber die Ursache ist geräteseitig: der
HTTPD-Task hing lange, weil der History-Writer (bzw. ein CSV-Import) den
`g_history_mutex` bei einem langen In-Place-Ring-Insert hielt. Drei Änderungen,
alle host-getestet:
1. **Begrenzter Mutex-Wait in den Lesepfaden.** `power_history_read`,
   `_read_window`, `_read_after`, `_aggregate`, `_has_minute` und
   `_get_status` nehmen den Mutex mit
   `pdMS_TO_TICKS(POWER_HISTORY_READ_LOCK_TIMEOUT_MS)` (800 ms) statt
   `portMAX_DELAY`. Der Writer und der Import behalten `portMAX_DELAY`.
   Folge: der HTTP-Task gibt auf und antwortet **schnell** statt bis zum
   Browser-Timeout zu hängen.
2. **Yields im Writer.** `shift_up_renumber()` ruft alle
   `POWER_HISTORY_SHIFT_YIELD_CHUNKS` (8) Chunks `vTaskDelay(1)`, damit der
   IDLE-Task (Watchdog) und gleich-/höherpriore Waiter laufen. Der Mutex bleibt
   gehalten, ein Leser wartet also höchstens eine Chunk-Runde.
3. **Handler vertragen `ESP_ERR_TIMEOUT`.** `power_history_api_handler` (JSON),
   `power_history_csv_handler` und der Aggregations-Zweig liefern bei
   `ESP_ERR_TIMEOUT` einen **leeren, gültigen** Datensatz (`storage_ready:false`)
   statt HTTP 500 / Hängen. Das Dashboard pollt weiter und erholt sich.
- **Kein Datenverlust beim Backfill (Kern-Fix).** In `history_sync.c` wurden die
  Resume-Cursor **vor** dem Merge fortgeschrieben. Ließ der Store-Puffer
  (`POWER_HISTORY_QUEUE_LENGTH` = 16) einen Tail fallen, sprang der Cursor
  darüber hinweg ⇒ Minuten waren endgültig verloren. Jetzt rückt der Cursor nur
  über **nachweislich gespeicherte** Minuten vor (bereits vorhandene +
  vom Merge akzeptierte). Bei einem Drop endet die Session, der Cursor bleibt vor
  der ersten fallengelassenen Minute und die nächste Session zieht sie nach.
- **Tests:** `test/test_history_sync.c` (+`test_no_loss_on_merge_drop`) und die
  neue Stress-Simulation `test/test_stress_history_sync.c` (in
  `run_host_tests.sh`): großer Sender-Ring, viele Sessions mit zufälligen
  Link-Abbrüchen **und** Store-Druck. Muss zu einem exakten Spiegel konvergieren:
  keine Minute verloren, keine Dublette, Cursor überspringt nie. Läuft grün
  (330 Checks, 0 Fehler). Bewusste Abwägung: ein **einzelner** Session-Lauf bricht
  bei Store-Druck ab (statt endlos zu retten); die Konvergenz passiert über die
  wiederkehrenden Sessions (Backfill-Worker alle `HISTORY_BACKFILL_PERIOD_MS`).
- **Nicht geändert** (bewusst): die Lesepfade halten den Mutex weiter für die
  Dauer eines Reads (bis 64 Records). Bei ~1,3 ms/Record auf SPIFFS bleibt das
  unter dem 800 ms-Budget. Ein Seqlock/Snapshot-Umbau war nicht nötig, weil der
  Writer zwischen den Chunks yielded.
- **Web-Health-Guard (2026-10-14, gegen „Web offline, kein Rollback").** Der
  OTA-Rollback (`boot_confirm.c`) deckt nur das **Boot-Fenster** ab: ein Image,
  das hochkommt und das Stabilitätsfenster überlebt, ist **confirmed** — danach
  prüft **niemand** mehr, ob der HTTPD noch antwortet. Genau das ist die Lücke
  (per USB geflasht gibt es ohnehin nichts zum Zurückrollen). Neuer Guard
  `main/web_guard.{c,h}` (Receiver-only, host-getestet `test/test_web_guard.c`):
  ein Task pingt alle 5 s die eigene `/healthz`-Route (Loopback `127.0.0.1`).
  Bleibt sie länger als `threshold_ms` (30 s) stumm, wird der Server neu
  gestartet (`httpd_stop`+`httpd_start`+Handler neu registrieren); hilft das
  binnen `reboot_timeout_ms` (90 s ab erstem Versuch) nicht, `esp_restart()` —
  dann greift wieder die Boot-Policy. **Sicherheitsnetz:** der Guard bleibt
  **passiv**, bis der Selbst-Probe **einmal** geklappt hat (`probe_works`); ein
  Build ohne funktionierenden Loopback rebootet kein gesundes Gerät. Status im
  Console-Kommando `status` (`web health: guarded/passive, last activity … ms,
  restarts …`). `max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - 3` (httpd keeps 3
  for itself); `CONFIG_LWIP_MAX_SOCKETS=16` gives 13 usable sockets (was 7 with
  the IDF default of 10, which let the pool run dry under the dashboard's
  parallel polls + the probe and produced `accept (23)` = ENFILE).

### Socket-Erschöpfung `accept (23)` (2026-10-22)
`httpd_accept_conn: error in accept (23)` ist **ENFILE**: der lwIP-Socket-Pool ist leer.
`start_webserver()` setzt `max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - 3` (httpd hält 3
Sockets selbst) — bei `CONFIG_LWIP_MAX_SOCKETS=10` also nur **7** nutzbar, und das reichte
unter den parallelen Dashboard-Polls + der Web-Probe nicht. `CONFIG_LWIP_MAX_SOCKETS=16`
in `sdkconfig.defaults` → 13 nutzbar (Commit `51a0ee4`). Die `104` daneben ist
**ECONNRESET** (Client gibt auf), die fehlgeschlagene Probe ist die Folge, nicht die Ursache.
`MEMP_NUM_NETCONN` folgt `CONFIG_LWIP_MAX_SOCKETS` (`lwipopts.h`), Kosten nur ein paar
hundert Byte statisches RAM.


## 🔨 BAUAUFTRAG (2026-10-14): History-Dubletten nach Backfill
**Status: Auftrag erteilt, noch NICHT gebaut.** Auftraggeber: Kontroll-Chat
(`cline/power-history-diag`). Dieser Abschnitt ist der verbindliche Auftrag.

### Symptom (aus echten Backups belegt)
`GET /api/power-history.csv` liefert **doppelte Minuten**. Empfänger-Export 14.10.:
4828 Zeilen / 3557 eindeutige Minuten → **1271 überzählige Zeilen**; ein früherer Export
615 doppelte Minuten / 1047 Zeilen. Die Dubletten-Paare liegen im `sequence` weit
auseinander (= zwei Zähler: Empfänger-Ring vs. Sender-Verlauf) → sie entstehen im
**Backfill-Merge**, nicht im CSV-Upload.

### Root Cause (im Host-Test reproduziert, 2026-10-14)
`main/power_history.c`:
- `minute_is_stored()` und der `power_history_read*`-Pfad nehmen an, der Ring sei
  **zeitlich sortiert** (`minute_is_stored` = **Binärsuche** über `oldest_slot..next_slot`).
- Der Backfill fügt **ältere** Minuten an (Sender-Verlauf) → Ring wird **unsortiert**.
- Folge: `minute_is_stored()` / `power_history_has_minute()` finden **vorhandene** Minuten
  nicht mehr (Binärsuche bricht ab) → `write_records()` hält sie für neu → **dieselbe
  Minute wird erneut geschrieben** → Dubletten. Auch der Export (`power_history_read`)
  überspringt beim Blocklesen unsortierte Einträge.
- Reproduktion (Host, `main/power_history.c` + eigener Treiber): 10 Live-Minuten enqueuen,
  dann 10 **ältere** Minuten mergen → `has_minute(T0)` = **0** (soll 1); dieselben
  Live-Minuten erneut mergen → `record_count` 20 → 30.

### Fix-Richtung (empfohlen: Option A)
**A) Ring zeit-sortiert halten (bevorzugt).** Beim Einfügen älterer Backfill-Minuten die
korrekte Slot-Position bestimmen und den Ring so umschaufeln, dass
`oldest_slot..next_slot` **aufsteigend** bleibt. Dann funktionieren Binärsuche (Dedup,
`read_window`) wieder. Betroffen: `write_records()` in `main/power_history.c` (gemeinsamer
Pfad für Live-Recorder **und** Merge). Achtung: Header (`record_count`/`next_slot`) und
beide Header-Kopien konsistent halten; O(n)-Umschreiben bei 29000 Records ist teuer → wenn
möglich Slots per `memmove`/Ringrotation verschieben.
**B) Alternative:** `minute_is_stored` + `read*` auf unsortierten Ring auslegen (lineare
Suche bzw. Sortierindex) — teurer im Betrieb, aber einfacher zu ändern.

### Mitnehmen
- **Dedup innerhalb eines Backfill-/Import-Batches**: `history_sync_run` filtert nur gegen
  `has()`; dieselbe Minute kann innerhalb einer Session doppelt ankommen (Retry/überlappende
  Seiten). Vor `merge()` im Batch deduplizieren.
- **Backfill-`errors` („Fehler: 2")**: bei `fetch()`-Fehlschlag (`sender_diag_exchange_raw`)
  bricht die Seite ab, Cursor retryt später. Prüfen, ob Fehler gezählt/erneut versucht werden.

### Regressionstest (muss mit)
Host-Test in `test/` (analog `test_history_backfill_merge.c`):
1. Live-Minuten T0..T9 enqueuen, dann ältere T-100..T-91 mergen.
2. Assert: `power_history_has_minute(T0) == true`, Ring zeit-sortiert.
3. Dieselben Live-Minuten erneut mergen → `record_count` **unverändert**.
4. CSV-Export (`power_history_read*`) liefert **keine** doppelten Minuten.
`test/run_host_tests.sh` muss grün bleiben; neuen Test dort einhängen.

### Constraints
- OTA-sicher (reine App-Änderung, keine `partitions.csv`/Bootloader/`sdkconfig.defaults`).
- CI grün. Nach dem Build: **Commit-SHA** an den Kontroll-Chat zurückmelden.

### ✅ CSV-Upload blockierte den HTTPD (BEHOBEN, 2026-10-18, PR #31 `be1507d`)
„Nach dem CSV-Upload bleibt das Interface minutenlang offline." Der Handler
`power_history_upload_handler` rebootete **nicht**; er streamte den Body und importierte
inline. **Ursache = HTTPD blockiert**, nicht Heap und nicht der Watchdog: `esp_http_server`
führt **jeden** URI-Handler auf **einem** Thread aus, und der alte Handler hielt den HTTP-Task
**und** `g_history_mutex` über die SPIFFS-In-Place-Inserts hinweg für Minuten — Dashboard-Poll,
`/healthz`-Selbst-Probe und Liveness-Feed stauten sich dahinter (der Import batchte schon in
64er-Blöcken, war also nie ein Heap-Problem).
Fix: (1) Body lesen/parsen in einem **eigenen Task** (`httpd_req_async_handler_begin`), der
HTTP-Thread kehrt sofort zurück; (2) der Task **parst nur** und übergibt jede sortierte
64er-Batch per **Queue** an den **Backfill-Worker** (der einzige History-Writer), der die
Queue zuerst leert und seine lange Sync-Session überspringt, solange `g_history_upload_active`
gilt; (3) neues reines, host-getestetes Modul `main/history_upload.{c,h}`
(`test/test_history_upload.c`). Dabei mitgefixt: `storage_ready` meldet jetzt echte
Persistenz (`power_history_status_t.persistent`, true nur wenn die SPIFFS-Datei offen ist,
nicht der RAM-Fallback), und Sender-Records ohne gültige Uhr (0 / 1970) werden in
`write_records`/`import_sorted` sowie vom CSV-Parser (pre-2024) verworfen.

## ➕ NACHTRAG (Kontroll-Chat, 2026-10-14): zwei Punkte für die Umsetzung
Der Bau-Chat hat den Auftrag übernommen (Radio-Manager verworfen — **richtig**, gehört nicht
hierzu). Zwei Ergänzungen:

1. **Kompatibilität mit bereits unsortiertem Ring.** Der Fix muss auch einen **bereits
   zerschossenen** Ring reparieren (das Gerät hat die Dubletten/Unsortierung ja schon). Also
   beim Fix nicht nur *neue* Inserts sortiert einfügen, sondern auch den vorhandenen,
   unsortierten Bestand in eine sortierte Ordnung überführen (z. B. einmalige
   Normalisierung/`history_csv_sort` beim Start oder beim ersten Insert). Sonst bleiben
   `has_minute`/Export für die Alt-Daten kaputt.
2. **Sequenz muss beim Umsortieren monoton bleiben.** `power_history_read(after_sequence=…)`
   (JSON + CSV-Export) und `power_history_read_after()` blättern über `sequence`. Wenn beim
   Umsortieren die `sequence` **neu** vergeben wird, sind alte Cursor/`after_sequence`
   **ungültig** (Export/HA-Dashboard verlieren Zeilen). Bitte `sequence` **als Identität
   erhalten** oder den Umbau so machen, dass die Export-Reihenfolge weiterhin monoton ist.

Beides ist Teil der Abnahme; der Regressionstest sollte (1) einen vorab unsortierten Ring und
(2) die Export-Monotonie abdecken.

### 📬 Austausch: `docs/CHAT-THREAD.md`
Zwischen Kontroll- und Bau-Chat gibt es keinen direkten Chat-Kanal (jeder Runtime sieht nur
die eigene Konversation). Der verbindliche, persistente Austausch läuft über
**`docs/CHAT-THREAD.md`** (nur anhängen, ein Commit pro Nachricht, sofort pushen). Bitte
Status/Fragen/SHA **dort** melden und vor der Arbeit einmal pullen, um die neuesten
Nachrichten zu sehen.

## Reverse-Kanal: Sandbox -> Odroid -> ESP32 (Geraet testen/flashen)
Der Sandbox hat keine LAN-Route (DS-Lite) und kein /dev/ttyUSB*; er kann das
Geraet nie direkt erreichen. Umgekehrt geht es: der Sandbox ist oeffentlich
(Port 12001 -> https://work-2-<RUNTIME_ID>.prod-runtime.all-hands.dev), der
Odroid pollt und fuehrt aus. Implementierung: /workspace/remote-shell/
(channel_server.py im Sandbox, odroid_agent.sh + install_odroid_agent.sh auf
dem Odroid, Key in KEY, Doku in README.md).
- Agent: CID=$(python3 /workspace/remote-shell/channel.py send "<cmd>"); python3 /workspace/remote-shell/channel.py wait $CID 30
- Odroid einmalig: sudo CHANNEL_KEY=<key> ./install_odroid_agent.sh (systemd
  esp32-channel, pollt alle 5 s). Skripte auch unter /files/odroid_agent.sh.
- Damit ist der serielle Web-Monitor (/console, POST /api/console) und der
  OTA-Pull (POST /api/ota-pull) am echten Geraet aus dem Chat bedienbar.

## ESP32-Fehlerdiagnose (Odroid, wiederkehrend)
- Skript: `/usr/local/sbin/esp32_diag.py` (stdlib, kein USB). Fragt den Empfaenger
  (`192.168.178.70`) per HTTP ab und zieht Sender-Daten ueber dessen Proxy
  (`/api/sender`), weil der Sender keine eigene IP hat.
- Units: `esp32-diag.timer` (alle 10 min, `OnBootSec=90s`) + `esp32-diag.service`
  (oneshot, `SuccessExitStatus=0 2`). Exit 2 = CRITICAL-Befund, kein Dienstfehler.
- Ausgabe: Journal (`journalctl -u esp32-diag`), State `/var/lib/esp32-diag/state.json`,
  Verlauf `/var/lib/esp32-diag/history.jsonl`. Manuell: `esp32_diag.py --print`.
- Befunde je Lauf: `receiver-unreachable` (CRIT), `sender-no-answer` (CRIT, /api/sender
  504), `sender-uart-dead` (CRIT, `uart_rx_bytes=0`), `no-sml-frames` (WARN),
  `version-skew` (WARN), `sender-rebooted`/`sender-queue-drops` (WARN, Zustandsvergleich).
- Schwellen: `TH_SENDER_SILENT_S=120`, `TH_HEAP_LOW=20 KiB`, `TH_STORAGE_DROPS_DELTA=50`,
  `TH_UPTIME_RESET_S=120`. Empfaenger-IP via `ESP32_RECEIVER` ueberschreibbar.
- OFFENER BEFUND (2026-10-21): `sender-uart-dead` — der Sender liest 0 Bytes vom
  SML-Zaehler; das ist der naechste zu behebende Bug (nicht der Reachability-Fix).

## Bootlog (/bootlog) — SML-Chatter unterdrueckt
- `main/boot_log.{c,h}` haelt die letzten ~6 kB Log in einem RAM-Ring; der Permalink
  ist `/api/bootlog` (Header `X-OTA-Key`).
- Pro CRC-gueltigem Telegramm loggt die Firmware `SML value: OBIS=... ` (main.c:2935).
  Diese hohe Rate fuellte den Ring in Sekunden und verdraengte die Startmeldungen.
- Loesung: `boot_log_suppress(marker)` (max. 4 Marker, Substring). `app_main` registriert
  `"SML value:"`; betroffene Zeilen bleiben auf der **Konsole**, fehlen aber im Ring.
- Nur genau dieser Praefix ist unterdrueckt — andere SML-Zeilen (Fehler, "batch ring
  full") bleiben im Bootlog. Host-Test: `test/test_boot_log.c`.

## BOOT-Taste: Config-Restore (3 s) / Firmware-Rollback (10 s) – **beide Rollen**
- Eine gelötete Taste am **BOOT-Pin** (`CONFIG_RECOVERY_BUTTON_GPIO`, Default **0
  für beide Rollen**; `-1` = aus) treibt zwei Aktionen, unterschieden nach
  Haltedauer **nach** dem Boot (der ROM sampelt den Strap-Pin beim Download-Mode,
  daher erst danach beobachten):
  - **≥3 s, dann loslassen** (`RECOVERY_BUTTON_RESTORE_MS`) → **vorherige Config
    zurückholen** und rebooten.
  - **≥10 s gehalten** (`RECOVERY_BUTTON_ROLLBACK_MS`) → **Firmware-Rollback**
    (`esp_ota_mark_app_invalid_rollback_and_reboot`), auch wenn das laufende Image
    schon bestätigt war.
- **Auch der Sender** honoriert die Taste: Er hat kein USB und ist sonst nur per
  ESP-NOW erreichbar, also ist die Taste dort der lokale Rückweg für ein Image,
  das nicht weit genug hochkommt, um das Relais-OTA (`/update-sender`) anzunehmen.
  Die Verdrahtung in `app_main` ist rollen-agnostisch; `recovery_start.c` gatet
  nur noch auf `CONFIG_RECOVERY_BUTTON_GPIO >= 0`, nicht mehr auf die Rolle.
  Eine nicht bestückte Taste (Pin hochgezogen) liest sich als „nicht gedrückt".
- **Restore feuert beim Loslassen**, nicht während des Haltens — sonst würde ein
  3-s-Halt rebooten, bevor die 10-s-Rollback erreichbar ist. Reine Logik in
  `main/recovery_button.{c,h}` (host-getestet, `test/test_recovery_button.c`);
  GPIO/OTA/Callback-Glue in `main/recovery_start.{c,h}` (beide Rollen; `-1` =
  Stub).
- **Zwei NVS-Slots im Namespace `rtcfg`:** `cfg` (committed) und **`prev`** (der
  zuletzt ersetzte committed Wert). Jedes committed `set` legt **vorher** `prev` ab
  (`runtime_config_store_prev`); die Taste/`restore-config` holen `prev` zurück.
  Das ist nötig, weil ein `set cpu-min 40` in NVS liegt und ein simpler Reboot es
  nur erneut anwendet.
- **Trial-Mode ist davon getrennt:** `trial/cand` hält den *Kandidaten* für
  `set trial …` und verwirft ihn bei Reset selbst (`main/trial_mode.{c,h}`). `prev`
  ist der Rückweg für ein *committed* `set` — die beiden nicht verwechseln.
- **Reihenfolge in `app_main`:** `recovery_set_restore_fn(...)` +
  `recovery_button_check()` laufen **nach `initialize_wifi_and_espnow()` und VOR
  `apply_power_management()`** — ein schlechter DFS-Boden hängt genau im Apply, also
  muss die Rettung vorher greifen. Nicht wieder hinter `boot_confirm_init()` schieben.
- Konsolenbefehl `restore-config` ruft denselben Pfad (Test/Remote). Ohne `prev`-Slot
  passiert **nichts** (kein Reboot, Gerät bleibt erreichbar).
- Grenzfall-Hinweis: `sdkconfig.receiver*` ist **nicht getrackt** (gitignore); die
  Schwellen kommen aus den Kconfig-Defaults.

## GitHub-Token des Odroid (Agenten)
- Secret-Datei: `/root/.config/github-bridge/token` (0600). Herkunft/Sinn unklar,
  Inhalt ist ein fine-grained PAT, login `christiansiebe`.
- Zugang fuer Agenten: `/etc/gitconfig` registriert den Credential-Helper
  `/root/.config/github-bridge/git-credential.sh` (liest nur die Datei; kein Secret in
  einer git-Config/URL). `/root/.bashrc` exportiert `GITHUB_TOKEN`/`GITHUB_WRITE_TOKEN`
  aus derselben Datei (Agenten-Shells lesen `.bashrc`, HOME ist leer).
- **Status (2026-10-21 23:xx):** Push **funktioniert wieder** (receive-pack 200,
  Branch `feat/boot-button-config-restore` gepusht) — der PAT hat offenbar wieder
  Schreibrechte bzw. wurde getauscht. Früher am selben Tag war er noch read-only
  (`git push` → HTTP 403). Wenn ein Push mit **403** scheitert, fehlt dem PAT
  unter *Contents: Read and write*; die Repo-`permissions` der API (`push:true`)
  sind die *Nutzer*-Rechte, nicht die Token-Scopes.

## Power-History: robust gegen kaputte Datenbank (2026-10-27, ungetaggt nach v0.1.1)
Ziel: **Eine kaputte/vollgeschriebene SPIFFS-DB darf das Geraet nie mehr
lahmlegen.** Boot, Web-UI, OTA, Telemetrie laufen immer; im DB-Fehlerfall sind
nur die DB-Werte weg (leer bzw. nur die neuesten Minuten), der Backfill vom
Sender fuellt nach. Ausloeser war ein echtes Hardware-Ereignis am Receiver:
die SPIFFS-Partition war korrupt/voll, **ein einzelnes `fseek` haengte >120 s**
und der Boot-Repair (`ring_heap_sort`, in-place auf SPIFFS) schickte das Geraet
in einen TWDT-Boot-Loop.
*(Diese Aenderung traegt noch keinen Tag; die geflashte Firmware meldet
`v0.1.1-<N>-g<sha>-dirty`. Seit `v0.1.1` liegen **2 `feat(...)`-Commits**
ungetaggt (BOOT-Button-Recovery) — nach `VERSIONING.md` ist das ein
**MINOR-Bump, also `v0.2.0`**, nicht ein PATCH. Die Robustheits-Aenderung
allein waere ein PATCH, aber ein Tag deckt den ganzen Baum ab.)*

### Die zwei Wurzeln (beide beseitigt)
1. **In-place-Disk-Heapsort beim Boot.** `ring_heap_sort`/`ring_sift_down`
   machten tausende `fseek`/`fread`/`fwrite` auf SPIFFS, mit einem Pacer, der
   nur bei Datentausch zaehlte - auf einem fast-sortierten grossen Ring kam der
   Watchdog nie dazwischen. **Geloescht** (Funktionen entfernt).
2. **Repair lief synchron im Boot (`power_history_init`).** Der HTTP-Server
   startet erst nach `power_history_init` - ein haengender Repair blockierte
   den ganzen Boot.

### Design (main/power_history.c)
- **`power_history_repair_ring()`** (public, nimmt den Store-Mutex): die einzige
  Reparatur-Stelle. Kleine Ringe (<= `POWER_HISTORY_REPAIR_RAM_LIMIT` 4096)
  werden in einem RAM-Buffer sortiert/dedupliziert (bestehender Pfad). **Zu
  grosse Ringe werden NICHT in-place sortiert, sondern auf die neuesten 4096
  Records gekuerzt** (RAM-Sort + Kompakt-Schreiben + `finish_repair`). Alte
  Minuten gehen verloren, der Sender-Backfill importiert sie wieder - ein
  laufendes Geraet mit kuerzerer Historie schlaegt einen keilenden.
- **Der Boot ruft den Repair nicht mehr auf.** Ein Hintergrund-Task
  `power_history_repair_worker` (Prio 1, Stack `POWER_HISTORY_REPAIR_STACK`
  3072) startet nach dem Writer und verzögert sich um
  `POWER_HISTORY_REPAIR_DELAY_MS` (10 s) - HTTP/OTA/Telemetrie sind laengst
  oben. **Bewusst NICHT am Task-Watchdog subscribed**: friert der Repair in
  einem haengenden SPIFFS-`lseek` ein, rebootet das Geraet nicht - es laeuft
  mit kaputter DB weiter, der naechste Boot versucht es erneut.
- Der Pacer (`repair_pacer_step`, Budget `POWER_HISTORY_REPAIR_BUDGET_MS`
  15000, Yield `POWER_HISTORY_REPAIR_YIELD_STEPS` 2048) bleibt in allen
  Repair-Schleifen.
- Tests: `test_history_ring_repair.c` ruft nach `power_history_init()` jetzt
  explizit `power_history_repair_ring()` (im Firmware-Lauf macht das der
  Background-Task). `test_full_ring_repair` prueft die neue Semantik: volle
  DB -> hoechstens `RAM_LIMIT` Records, sortiert, dedupliziert, neueste Minute
  erreichbar. `run_host_tests.sh` gruen.

### Erfahrungen aus dem echten Fall (Receiver, 2026-10-26/27)
- **Diagnose eines haengenden Boots:** nicht nur `ping`/HTTP prüfen - die
  serielle Konsole zeigt den TWDT-Abort (`task_wdt: Aborting.` + Backtrace).
  `addr2line` (aarch64) auf die Backtrace-Adressen gegen die `.elf` nennt die
  exakte Stelle (hier: `SPIFFS_lseek` -> `spiffs_obj_lu_find_id_and_span` ->
  Endlossuche in der Objekt-Table).
- **Rettungsweg fuer einen gebrickten Receiver (beide OTA-Slots booten nicht):**
  USB-TTL-Adapter (CP2102) an den Receiver, `esptool --no-stub` (Stub startet
  auf diesem Chip nicht), dann `erase_region 0x320000 0xE0000` (nur die
  Storage-Partition!) und die App neu schreiben - die History-Partition ist
  gemeinsam, die App-Slots sind getrennt. Der Backfill vom Sender fuellt die
  geleerte DB wieder.
- **Sender-Telemetrie nach DB-Fehler:** Der Receiver bekommt `caught_up:
  True` mit dem Sender, sobald die DB leer/frisch ist; der Backfill laeuft nur
  bei Luecken und ist der Grund fuer die frueheren API-Timeouts (splices).

## WW-Ladung startet nicht: Sollwert-Sprung statt charge=start (2026-10-28)
Symptom: `extra_warmwasser`-Schalter an, aber es wird nicht geheizt - Zylinder
faellt sogar (38,2 -> 33,9 C). Der Verdichter lief nur 3-7 min und ging dann auf
13-22 W zurueck (gemessen 10:33-10:36, 11:48-11:51, 12:02-12:27, 12:45-12:49).
`activeHeatSource` sprang dabei auf `none` bzw. `appliance_only` (Gas).

**Ursache - zwei Dinge kommen zusammen:**
1. `dhwCircuits/dhw1/charge` = `stop` und `currentSetpoint` = 0.0. Der
   Ladungsbefehl wurde von der Anlage nicht als neue Waermeanforderung
   aufgenommen. Ein Sollwert, der (scheinbar) schon erfuellt ist, loest keine
   Ladung aus - auch `charge=start` allein greift nicht zuverlaessig.
2. Der **Vorlauf lag UNTER dem Zylinder** (Vorlauf 29-31 C, Zylinder 35-38 C) =
   negativer Hub. Die WP kann den Speicher so nicht laden; die Automation
   `cop_ww_hub_waechter` ueberwacht genau diese 2-K-Grenze.

**Was zuverlaessig wirkt (Nutzer-Ansatz, verifiziert):**
`temperatureLevels/low` einmal auf 30 und dann auf 47 setzen (je PUT -> 204).
Der Sprung erzeugt eine NEUE Anforderung. Messung 13:58 -> 14:42:
Zylinder 33,4 -> 36,2 C, Vorlauf 33,7 -> 40,5 C (Hub ~4 K), Verdichter
843 -> 2103 W, `activeHeatSource` = heatpump_only. Laeuft stabil durch.
Also: **Sollwert-Sprung (entweder `singleChargeSetpoint` oder
`temperatureLevels/low`) ist der Ausloeser, nicht `charge`.**

**Fallen/Beobachtungen:**
- `singleChargeSetpoint` (= `number...solltemperatur_extra_warmwasser`, Ziel der
  Einmalladung) stand zeitweise auf **30** - das ist UNTER der Zylindertemperatur
  und legt die Extra-WW-Funktion lahm. Gehoert auf 48. Nach dem Test pruefen.
- `chargeDuration` ist eine FESTE Laufzeit, kein Zielwert (Werkswert 30 min lief
  waehrend der Anti-Takt-Sperre ab, ohne dass geheizt wurde). Steht jetzt auf 180.
- Die Anlage fuehrt WW-Programm A: `11:00=low, 16:00=off` (alle Tage), `low`=48,
  `high`=50. `operationMode` = ownprogram.

**Optimierungs-Stand (gemessen, NICHTS geschrieben):**
- `controlStrategy` = `cost` (richtig fuer Bivalenz). Wichtig: Bei
  `maxRenewableShare` verschwinden die `energyTariff`-Ressourcen (404) und die
  Tarif-Entitaeten werden unavailable. Zurueck auf `cost` -> Tarife sofort wieder
  da (28/9/17 ct). Die Anlage hatte am 27.10. 23:01 selbst umgestellt.
- hc2: `controlType` = wdcoptimized, `setpointOptimization` = **on** (gut),
  `maxFlowTemp` 40 C, `suWiThreshold` 15, `suWiSwitchMode` automatic.
- hc1: wdcsimplified, maxFlowTemp 53, suWiThreshold 15, automatic.
- hc3: wdcsimplified, maxFlowTemp 55, suWiSwitchMode **off**.
- Zeitprogramme: hc2 06:00=21,5 / 23:00=21,0 | hc1 08:00=20 / 23:00=19 |
  hc3 08:00=19 / 23:00=19. Der Morgen-Peak ist der Sprung 06:00/08:00.
- `silentMode` = off, `powerReduction` = 30 %, `lowNoise` = inactive.
- Gesperrt (403, Kontoberechtigung): `heatCurve`, `roomSetpoint`,
  `awayMode`, `bivalentPoint`, `switchoverTemp`, `dhwPriority`.
- `hc2/setpointOptimization` ist der eingebaute Glaettungshebel (Anlage faehrt
  den Sollwert vor dem Programmpunkt langsam an) - steht bereits auf `on`.

## Silent-Mode-Drosselung bei WW-Ladung: Vorlauf bleibt oben, Wasser zieht hoch (2026-10-28)
Nutzer-Beobachtung: "Ich hab zum Ende 60% Leistung reduziert, dabei bleibt Vorlauf
auf 51 grad und Wasser zieht hoch." **Bestaetigt - und es ist ein nutzbares
Steuersignal.**

**Messung (Ladung 14:20-15:56, Anlagen-eigene Waermemengenzaehler):**
```
Fenster              Verd(avg)  Vorlauf  Zylinder  dTherm dElek  COP   P_th
volle Leistung        2481 W     48.9     44.4     2.49   0.77   3.23  9.34 kW
60% gedrosselt        1767 W     50.9     45.9     1.05   0.32   3.28  6.30 kW
```
- Der **Vorlauf stieg** trotz Drosselung von 48,9 auf 50,9 C (Hub 4,5 -> 5,0 K).
  Der Zylinder zog weiter hoch (44,4 -> 45,9, am Ende 47,9 C).
- Die **Leistungsreduzierung drosselt den Verdichter, nicht den Vorlauf**: bei
  halber Verdichterleistung bleibt der Vorlauf gleich oder steigt, weil die
  Waermemenge pro Zeit sinkt und die Anlage laenger auf Temperatur faehrt.
- **COP blieb praktisch gleich (3,23 -> 3,28).** Die Drosselung kostet also
  keinen COP, sie **streckt nur die Laufzeit** (P_th 9,34 -> 6,30 kW).

**Wichtig - Semantik von powerReduction:** Der Wert ist der PROZENTSATZ DER
REDUZIERUNG, nicht die Restleistung. `60` = staerker gedrosselt als `30`.
Gegengeprueft: Red=60 -> 1205 W, Red=50 -> 756 W, Red=30 (silent off) -> 2666 W.
**Der Wert wirkt nur, wenn `silentMode/enabled` = `on` ist** - steht silentMode
auf `off`, ist `powerReduction` wirkungslos (Anlage faehrt voll).

**Anlagen-eigenes Rueckmelde-Signal (neu gefunden):**
`binary_sensor.mx_400_hybman_compressor_speed_limited_due_to_silent_mode`
-> `on`, sobald die Anlage den Verdichter wegen Silent Mode begrenzt
(15:45-15:55 on, passend zur Drosselung). Dazu
`binary_sensor.mx_400_hybman_low_noise_mode_active`. Das ist die **bestaetigte**
Rueckmeldung, dass die Drosselung angekommen ist - besser als der Sollwert allein.

**Konflikt in der bestehenden Automation `cop_hc2_raum_und_pv` (gefunden):**
```
limit = (not ww_running) and importing and (...)
red   = red_base if limit else red_pv     # red_pv = 30
mode  = 'on' if limit else 'off'
```
Waehrend einer WW-Ladung ist `ww_running` True -> `limit` False -> es wird
`powerReduction=30` UND `silentMode=off` geschrieben, also **Drosselung
aufgehoben**. Genau das war beim Nutzer-Eingriff zu sehen: die Werte pendelten
15:44-16:36 zwischen 30/40/50/60 und silent on/off, weil Automation und
Nutzer-Eingriff gegeneinander schrieben. Die Automation behandelt WW als
"Nicht-Drosseln"-Fall - obwohl gerade WW der Fall ist, wo Drosselung nuetzt
(COP gleich, laengerer Lauf, weniger Spitzenlast).

**Nutzbar fuer die Steuerung:** Bei WW-Ladung ist die Drosselung ein
**kostenloser Laufzeit-Strecker** (COP 3,23 -> 3,28, keine Einbusse) und
glaettet die Verdichterspitze (2705 -> 1159 W). Fuer PV-Ueberschuss ist das
sogar ideal: weniger Netzbezug pro Minute, mehr Stunden mit Eigenverbrauch.
`cop_ww_hub_waechter` hebt die Drosselung dagegen ABSICHTLICH auf, wenn der Hub
unter 2 K faellt - das ist der richtige Schutz, aber er greift hier nicht, weil
der Hub bei 4,5-5,0 K lag.

**Wichtig zur Messaufloesung:** Die Waermemengenzaehler liefern nur alle
~305 s einen Wert, Vorlauf/Zylinder alle ~625 s. Kurze Fenster (<5 min) sind
damit NICHT belastbar. Verdichterleistung kommt alle 61 s.

## Wetterbasierte WW-Startverschiebung: gemessen, 14:30 ist zu spaet (2026-10-28)
Frage des Nutzers: "Wenn erst nachmittags die Sonne kommt, schieben wir WW-Start
bis spaetestens 14:30 nach hinten?" **Antwort: die Idee ist richtig gedacht,
aber 14:30 liegt hinter dem Ende des Ueberschussfensters.**

**Was an Wetterdaten da ist:**
- `weather.forecast_home` (met.no), **48 h stuendlich**, mit `cloud_coverage`
  und `uv_index`. Abruf: `POST /api/services/weather/get_forecasts`
  `{"entity_id":"weather.forecast_home","type":"hourly"}`.
- **KEINE PV-Prognose** installiert: kein forecast.solar, kein Solcast
  (Integrationsliste enthaelt nur `met`, `sun`, `template`, ...).
- **Die Anlage hat ein PV-Signal, aber gesperrt (403):**
  `pv/surplusAvailable`, `pv/power`, `pv/status`, `pv/setpoint`,
  `heatSources/hybrid/pvSurplus`, `hc*/solarInfluence` - alle 403. Sie kann es
  also nicht selbst.
- **Vorhersage-Guete ist NICHT pruefbar:** `weather.*` hat 0 Eintraege in
  `statistics` und nur den aktuellen Zustand in `states`. Es gibt keine
  Historie der Vorhersagen, also laesst sich rueckwirkend nicht sagen, wie gut
  met.no die Bewoelkung hier trifft. Ehrliche Grenze.

**Der harte Befund (10 Tage echte Messung, `stromzaehler_leistung_15_min_mittel`):**
Ueberschuss = letzter Wert <= -800 W (also >=800 W Einspeisung):
```
 Tag     Fensterende   spaetester Start (90-min-Ladung)
 20.10     14:51          13:21
 22.10     12:59          11:29
 23.10     14:44          13:14
 24.10     15:40          14:10
 25.10     14:19          12:49
 26.10     13:54          12:24
 27.10     12:20          10:50
 28.10     13:31          12:01
 Median-Ende 14:19 | spaetestes Ende 15:40 | fruehestes 12:20
```
**0 von 7 Tagen erlauben einen Start um 14:30** (bei 90 min Ladung). Selbst der
beste Tag (24.10., Ende 15:40) erlaubt nur 14:10.

**Der Grund ist Sonnengeometrie, nicht Wetter.** 10-Tage-Mittel Netz je Stunde:
```
 09h -470 | 10h -582 | 11h -1114 | 12h -1482 | 13h -140 | 14h -458
 15h +129 | 16h +327 | 17h +888 | 18h +916 | 19h +1743   (negativ = Ueberschuss)
```
Ab 13h bricht der Ueberschuss ein und kippt ab 15h in Bezug. Im Oktober steht
die Sonne dann zu tief, waehrend der Hausverbrauch abends steigt. **Das laesst
sich nicht wegwarten.**

**"Sonne erst nachmittags" ist im Oktober selten UND dann schwach:**
Von 9 auswertbaren Tagen: 1 Tag "NUR NACHMITTAG" (24.10.), 2 "nur VORMITTAG"
(25./28.10.), 1 "beide gut" (20.10.), 5 "beide schlecht". Die Wetterlage, fuer
die die Idee gedacht ist, trat **einmal in 10 Tagen** auf - und am 24.10. lag
das Fenster 13-15h, also haette ein 14:30-Start wieder nicht gepasst.
Beispiel 30.10. (Vorhersage): Vormittag Nebel 100 %, Nachmittag 14-15h sonnig -
aber UV nur 0,8/0,4, also **kaum Ertrag**. Ein Nachmittag im Oktober bringt
wenig Ueberschuss, auch wenn die Sonne scheint.

**Wichtiger Nebenfund - Bug in `cop_ww_start_optimal`:**
Die Automation heisst "WW-Start bei PV-Ueberschuss", hat aber **KEINEN
Einschalt-Zweig**. Der `choose` enthaelt genau eine Bedingung mit
`switch.turn_off`. In der ganzen `automations.yaml` gibt es **kein einziges
`switch.turn_on`** (`grep -c` = 0). Sie schaltet also nur AB, nie AN - passend
dazu, dass der Nutzer den Extra-WW-Ansatz verworfen hat. Der Name ist
irrefuehrend; die Startfunktion fehlt komplett.

**Bessere Loesung als 14:30 (Vorschlag, nicht umgesetzt):**
Das WW-Programm laeuft ohnehin 11:00=low(48) - und die Messung zeigt, dass die
Ladungen real 10-13h starten, also **schon im PV-Fenster**. Die Verschiebung
sollte deshalb in beide Richtungen gehen und **gedeckelt** sein:
- Guter Vormittag erwartet -> frueh laden (10:00), statt auf den Nachmittag zu
  warten.
- trueber Vormittag, Besserung ab Mittag -> nach hinten, aber **maximal auf
  ~12:30** (nicht 14:30), damit 90-115 min Ladung noch im Fenster endet.
- Fensterende aus der Vorhersage schaetzen (Stunden mit hohem UV und niedriger
  Bewoelkung) und den Start als `fensterende - 120 min` setzen.
- Mit Silent-Mode-Drosselung dauert die Ladung 115 statt 60 min -> der
  spaeteste Start rueckt NOCH weiter nach vorne (siehe Abschnitt Silent-Mode).

## WW-Planung nach Wetterbericht: umgesetzt + Dashboard (2026-10-28)

**Auftrag:** Warmwasser nicht nach fester Uhrzeit, sondern in das PV-Fenster
legen, und im Dashboard sichtbar machen. **Erledigt.**

### Aufbau (4 Vorhersage-Sensoren + 2 Automationen + 6 Helfer)
- `templates.yaml` (ab ~Zeile 246): trigger-basierte Sensoren, die aus
  `weather.forecast_home` (met.no, 48 h stuendlich) rechnen:
  * `sensor.ww_plan_fenster_start` / `_fenster_ende` - die PV-Stunden
    (UV >= 0,3 UND Bewoelkung <= 70 %).
  * `sensor.ww_plan_ziel_start` - `fensterende - 120 min`, gedeckelt
    **10:00-12:30**. Gemessen (10 Tage): das Ueberschussfenster endet im
    Median um **14:19**, fruehestens 12:20 -> ein Start nach 12:30 waere
    nicht mehr im Fenster. Ohne erkennbares Fenster: 11:00.
  * `sensor.ww_plan_status` - Lage als Text ("2 PV-Stunden, Ertrag-Index 0.7").
- `automations.yaml`:
  * `cop_ww_plan_start` (Alias "WW-Plan Start (Wetterbericht + Sollwert-Sprung)")
  * `cop_ww_plan_drossel` (Alias "WW-Plan Drosselung (Flag fuer hc2)")
- Helfer: `input_boolean.ww_plan_aktiv` (on), `input_boolean.ww_plan_drossel_aktiv`,
  `input_number.ww_plan_zieltemperatur` (48), `input_number.ww_plan_ladedauer` (180),
  `input_text.ww_plan_meldung`, `input_datetime.ww_plan_letzter_start`.

### Warum Sollwert-Sprung statt charge=start
Ein Sollwert, der schon erfuellt scheint, loest **keine** Ladung aus - auch
`charge=start` nicht. Der Sprung `temperatureLevels/low` erst auf **30**, 8 s
warten, dann auf die Zieltemperatur erzeugt eine neue Waermeanforderung
(verifiziert 28.10.: Zylinder 33,4 -> 47,9 C, Verdichter bis 2,7 kW).

### Nur EINE Schreibstelle fuer Silent Mode / Leistungsreduzierung
`cop_hc2_raum_und_pv` schreibt alle 2 min Silent Mode und Leistungsreduzierung.
Eine zweite Automation, die dasselbe schreibt, wuerde sich mit ihr **bekaempfen**.
Deshalb setzt `cop_ww_plan_drossel` **nur das Flag** `ww_plan_drossel_aktiv`;
hc2 wertet es aus:
```
ww_hub_ok    = (Vorlauf - Zylinder) >= 2.0
ww_throttle  = ww_laeuft and ww_plan_drossel_aktiv and ww_hub_ok
red          = min(red_base, red_phys) if limit else (drosselung_bei_ww if ww_throttle else red_pv)
mode         = 'on' if (limit or ww_throttle) else 'off'
```
Der Hub-Schutz (>= 2 K) ist doppelt abgesichert: in hc2 **und** in
`cop_ww_hub_waechter`. Grund (Messung 28.10.): bei 60 % Reduzierung bleibt der
COP praktisch gleich (3,23 -> 3,28), die Verdichterspitze faellt von 2705 auf
1159 W - ideal fuer PV-Eigenverbrauch, aber nur solange der Vorlauf den Zylinder
hochziehen kann.

### `shell_command` wirft NICHT bei Exit-Code != 0 (wichtiger Fallstrick)
Live nachgestellt: bei ausgefallener Cloud (HTTP 504) scheiterten alle drei
`pointt_set`-Aufrufe, **der Lauf ging trotzdem bis zum Ende** und setzte den
Tagesmarker - ein ganzer Tag ohne Warmwasserladung, ohne Fehler im Ablauf.
Ursache im Code: `shell_command/__init__.py` **loggt** nur
`Error running command ... return code: N` und kehrt normal zurueck
(`SupportsResponse.OPTIONAL`, aber kein `raise`).
**Fix:** `response_variable` auswerten und mit `stop:` abbrechen:
```
- service: shell_command.pointt_set
  data: {path: ..., value: ...}
  response_variable: w_laden
- if:
  - condition: template
    value_template: '{{ w_laden.returncode | default(1) != 0 or ... }}'
  then:
  - stop: Schreibfehler - Tagesslot bleibt frei
```
Der Marker wird **ganz zuletzt** gesetzt. Verifiziert: bei 504 bleibt der Slot
frei, die Meldung nennt die Rueckgabecodes (`Ladedauer 1, Sollwert 1`).

### Tagesmarker: 00:00:00 zaehlt als NICHT gestartet
Ein frisch angelegter `input_datetime`-Helfer steht auf dem **Erstellungstag um
Mitternacht**. Ein reiner Tagesvergleich haette den ersten Start blockiert.
Jetzt:
`letzter_start[:10] != heute or letzter_start[11:19] == '00:00:00'`.

### Dashboard
Neuer Abschnitt **"WW-Planung (Wetterbericht)"** im Dashboard `wp-native`
(View "Waermepumpe", sections-Layout), eingefuegt an Position 2 - direkt nach
"Temperaturen". Vier Karten: Markdown-Erklaerung, "Heute" (Plan, Fenster, Lage,
letzter Start, Meldung, **Anlage erreichbar (PointT)**), "Einstellungen"
(Schalter + Zieltemperatur + Ladedauer), Verlauf "Zylinder + Vorlauf (24 h)".
Der Cloud-Status steht bewusst mit dabei - faellt die Bosch-Cloud aus, sieht
man das sofort an dieser Zeile (`unavailable`).

**Wichtig - Dashboard-Speicherung:** `.storage/lovelace.wp_native` direkt zu
editieren genuegt NICHT, HA liefert die **gecachte** Config (im Browser fehlte
der Abschnitt). Richtig ist der WebSocket-Aufruf
`{"type":"lovelace/config/save","url_path":"wp-native","config":<config>}`.
Danach ist der Abschnitt sofort live.

### Verifikation (alles ohne echten Reboot, HA 2026.10.4)
- `homeassistant/check_config` -> 200, keine Fehler im Log.
- Alle 12 referenzierten Dashboard-Entitaeten existieren (`/api/states`).
- Rendering headless (Firefox ESR + geckodriver): Abschnitt und **alle**
  Beschriftungen im DOM, 231 Textelemente, Verlaufs-Karte mit Titel
  "Zylinder + Vorlauf (24 h)", **keine** Fehler-/Leer-Texte aus dem Abschnitt.
- Fehlerpfad scharf getestet (Cloud auf 504): Marker blieb frei, Meldung
  geschrieben, `stop` griff.
- Keine Zombie-Prozesse nach den Browsertests.

### WICHTIGER NEBENBEFUND: die Bosch-Cloud (PointT) war den ganzen Tag gestoert
`https://pointt-api.bosch-thermotechnology.com/pointt-api/api/v1` antwortet seit
**11:44** durchgehend **HTTP 504 (Gateway Timeout)** - reproduziert um 18:13,
18:19, 18:22 und 18:26. Folge: **alle 158** `bosch_buderus_heating`-Entitaeten
sind `unavailable` (auch Silent Mode und Leistungsreduzierung), die Integration
hat um 18:19 ihren Circuit Breaker fuer 300 s geoeffnet.
**Das ist NICHT durch diese Arbeit entstanden** - die ersten 504-Ausfaelle
stehen um 11:44 im Log, die Aenderungen liefen ab ~18:05. Die lokale
`bosch_k40rf`-Integration liest weiter (124 von 186 Entitaeten verfuegbar).
Wer schreiben will, muss warten, bis die Cloud sich erholt; die Automation
versucht es alle 2 min und laesst den Tagesslot so lange frei.

### Backups
`/root/backup-ww-plan-20261028/`: `automations.yaml.vor-ww-plan`,
`.vor-konfliktfix`, `.vor-robustheit`, `.vor-returncode-fix`,
`lovelace.wp_native.vor-ww-plan`, `input_boolean`, `input_number`, `input_text`,
`input_datetime`.

## Cloud-Ausfall war voruebergehend + 21 Steuer-Entitaeten fehlen (2026-10-28/29)

Zwei Fragen des Nutzers: (1) warum ist die Cloud nicht mehr schreibbar,
(2) warum nicht um 8/9 Uhr starten. Beide beantwortet, **nichts geaendert**.

### 1. Die Cloud ist wieder da - der 504 war voruebergehend
Gemessen um 23:16-23:40: **alle** Endpunkte wieder 200/204, Schreiben
funktioniert (`PUT /system/silentMode/powerReduction` -> **204**,
`PUT /heatingCircuits/hc2/setpointOptimization` -> **204**). Die Integration
hat sich selbst erholt: **158 -> 21** unavailable, `requests_by_status_class`
= `{"2xx": 57}`, `success_rate_percent: 100.0`, `requests_failed: 0`.
Es war also ein **Bosch-seitiger Ausfall** (Gateway-Timeout), kein Defekt hier.

**Nebenbefund: der Bulk-Lesepfad ist fuer das Konto gesperrt.**
`POST /gateways/<gw>/bulk` -> **403 Forbidden** (HTML-Fehlerseite von Bosch).
Einzelressourcen (`/gateways/<gw>/resource/<pfad>`) gehen weiter. Die
Integration faellt darauf zurueck (21 einzelne `resource`-Requests im Log).

### 2. Der eigentliche Fund: 21 Steuer-Entitaeten sind dauerhaft `unavailable`
Darunter genau die, die die HK2-Regelung braucht:
```
number...heizkreis_2_temporary_room_setpoint   <- schreibt cop_hc2_raum_und_pv
select...heizkreis_2_sollwertoptimierung
select...heizkreis_2_regelungsart
select...warmeerzeuger_control_strategy
number...heizkreis_{1,2,3}_su_wi_threshold
number...anlage_{gas,pv,electricity,duration}
select...anlage_mode, heizkreis_1_*, warmeerzeuger_reminder_enable
sensor...heizkreis_3_su_wi_threshold
```
**Das ist NICHT die Cloud.** Beweis: die **Lese-Sensoren derselben Pfade
liefern Werte** (`sensor.buderus_mx400_heizkreis_2_temporary_room_setpoint`
= 21.5, `..._sollwertoptimierung` = on), und die Diagnose sagt
`bulk_result: success` + `discovered: true` fuer
`/heatingCircuits/hc2/temporaryRoomSetpoint`.

**Ursache: die Integration wurde heute aktualisiert - und dabei ist die
Schreib-Allowlist geschrumpft.** HACS hat am **28.10. 09:47** auf **v0.8.2**
gehoben (vorher 0.8.1, `manifest.json` mtime 09:47). Danach wurden
`writes.py` (12:46), `number.py` (12:47) und `resource_catalog.py` (12:55)
nochmals angefasst. Messung der `NumberWritePolicy`-Eintraege:

| Version | NumberWritePolicy |
|---|---|
| 0.8.1 | 5 |
| 0.8.2 | 9 |

**Aber: keiner der neuen Eintraege deckt `temporaryRoomSetpoint`,
`suWiThreshold`, `controlType`, `setpointOptimization`, `controlStrategy`
oder `energyTariff` ab** - `grep` in beiden Versionen: **0 Treffer**.
`assess_control()` gibt fuer diese Pfade `no_scalar_control_policy` zurueck,
und `available` verlangt eine passende Policy
(`number.py:149`, `select.py:204`) -> **Entitaet wird `unavailable`**.

**Wichtig - der Zustand ist neu, nicht dauerhaft.** Die Historie zeigt, dass
diese Entitaeten **bis heute 11:57 Werte hatten** (z. B.
`heizkreis_2_temporary_room_setpoint`: 21.5/22.0/22.5 bis 11:48, letzter Wert
11:48:14). Erst danach kippt es dauerhaft auf `unavailable`. Die Entitaeten
existieren noch als **Waisen** im Register (`restored: true`, min/max/step
bekannt), aber ohne Schreib-Policy.

**Folge fuer die Regelung (wichtig):** `cop_hc2_raum_und_pv` ruft
`number.set_value` auf diese Entitaet. HA loggt seit 11:52 **82x**
`Referenced entities number.haus_buderus_mx400_heizkreis_2_temporary_room_setpoint
are missing or not currently available`. Der Aufruf laeuft also **ins Leere** -
die Raum-Sollwert-Anhebung passiert derzeit **nicht**. Die Silent-Mode-/
Leistungsreduzierungs-Pfade sind **davon nicht betroffen** (eigene Policy,
`select.buderus_mx400_anlage_silent_mode` = on, `number...leistungsreduzierung`
= 40.0, beide verfuegbar).

**Lokale Abweichungen von Upstream 0.8.2** (per `diff` gegen das Tag geprueft):
`writes.py` 9 Zeilen, `number.py` 1, `resource_catalog.py` 7 - alle drei
ergaenzen **nur** den `powerReduction`-Pfad (`integerValue` + `%`-Einheit +
Namens-Tail). Also eigene Anpassungen, die den Verlust nicht erklaeren.

**Rueckweg / Handlungsoptionen (noch NICHT gemacht):**
- Auf **0.8.1** zurueck (HACS) - dann sind die alten Policies wieder da.
- Oder die fehlenden Pfade in `writes.py` ergaenzen (`EnumWritePolicy` bzw.
  `NumberWritePolicy` + `_number_identity`-Tail). **Achtung:** `_number_identity`
  ist ein `{...}[tail]`-Dict - ein unbekannter Tail wirft `KeyError` und reisst
  das ganze Number-Setup ab.
- Die Regelung bis dahin **nicht** auf die tote Entitaet schreiben lassen
  (sonst nur Log-Spam; ein Schaden entsteht nicht, es passiert einfach nichts).

### 3. Startzeit: 8/9 Uhr ist messbar schlechter - aber die Formel ist es auch
Frage war, ob WW bei starker Morgensonne schon um 8 oder 9 starten soll.
Gemessen an **8 Tagen mit echter Einspeisung** (`sensor.esp32_ir_stromzaehler_
leistung`, Minutenraster, Fenster = Einspeisung >= 800 W), Ladedauer 150 min:

| Start | PV-Abdeckung der Ladung (Mittel) | Median |
|---|---|---|
| **08:00** | **14 %** | 14 % |
| 09:00 | 34 % | 40 % |
| 10:00 (IST, Deckel) | 50 % | 45 % |
| **Fensterende - 150 min** | **63 %** | 62 % |
| Tagesoptimum | 74 % | 75 % |

**Das PV-Fenster beginnt im Median erst um 09:43** (fruehestens 08:55,
spaetestens 12:12). Ein Start um 08:00 liegt an **0 von 8 Tagen** im Fenster,
um 09:00 an **1 von 8**. Der Nutzerwunsch "Sonne so frueh" trifft auf die
Oktober-Realitaet nicht zu - das Fenster ist im Herbst spaet und kurz.

**Aber der zweite Teil der Frage stimmt:** der **Deckel 12:30 kostet Abdeckung.**
Ohne Deckel steigt der Mittelwert auf **61-66 %** (Deckel: 63 %) und der Median
von 62 auf **66 %**; die harten 10:00 sind mit 50 % deutlich schlechter. Der
Deckel ist nur noetig, damit die Ladung noch im Fenster endet - er ist aber
etwas zu streng, wenn das Fenster bis 15:42 reicht (24.10.).

**Und: die Raeume sind morgens NICHT warm, sondern am kaeltesten.**
`sensor.fussboden_room_temperature` (bzw. `..._heizkreis_2_raumtemperatur`),
Stundenmittel: 06h 20.85, 07h 21.10, 08h 21.00, 09h 21.00 gegen **14-16h 21.63-
21.72**. Die Morgenstunden sind das **Tagestief** (Nachtabsenkung). Ein
WW-Start um 8/9 wuerde also mit der Heizung um dieselbe knappe Vorlauftemperatur
konkurrieren, statt sie zu nutzen.

**Empfehlung (nicht umgesetzt, auf Wunsch):** Startzeit weiter aus der
**Vorhersage** ableiten (das tut die Automation schon), aber
1. den **Deckel 12:30 auf ~13:00-13:30 lockern** (kostet nichts, bringt
   ~4 % Median-Abdeckung),
2. bei **Spaetstart-Tagen** (Fenster beginnt erst nach 12:00, wie 24.10.) die
   Ladung eher **verkuerzen** als spaet zu starten,
3. den **Morgen nicht als WW-Fenster** einplanen - dort ist es dunkel UND der
   Raum kalt. Der Nutzen einer WW-Verschiebung liegt in den Schultermonaten,
   nicht im Herbstmorgen.

### Der Ausweg, verifiziert: direkt an der PointT-API geht es
Der Pfad ist **nicht** gesperrt - nur HA's Entitaet ist es. No-Op-Schreibtest
(identischer Wert, aendert nichts):
```
PUT /gateways/102090608/resource/heatingCircuits/hc2/temporaryRoomSetpoint
    {"value": 21.5}                       -> 204
PUT /gateways/102090608/resource/heatingCircuits/hc2/setpointOptimization
    {"value": "on"}                       -> 204
```
**Das heisst:** die HK2-Raum-Sollwert-Anhebung kann weiter funktionieren, wenn
sie statt `number.set_value` den schon vorhandenen Umweg
`shell_command.pointt_set` benutzt (`path: heatingCircuits/hc2/temporaryRoomSetpoint`).
Das ist derselbe Mechanismus, den die WW-Automation fuer
`dhwCircuits/dhw1/temperatureLevels/low` nutzt - und dort ist die
Rueckgabewert-Pruefung (`response_variable` + `stop`) schon eingebaut.
**Nicht umgesetzt** - das ist eine Entscheidung des Nutzers (siehe Frage oben).
