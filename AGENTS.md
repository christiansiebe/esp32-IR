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
- CPU ceiling is **240 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` in
  `sdkconfig.defaults`), scaled down by DFS to the 40 MHz XTAL when idle. The
  ceiling is **single-sourced** in `CONFIG_ESPNOW_CPU_MAX_MHZ`
  (`main/Kconfig.projbuild`, range 80..240, default 240): `apply_power_management()`
  in `main.c` applies it for **both** roles (light sleep off for the receiver,
  which must stay responsive for HTTP + ESP-NOW) and `uart_sleep_window.c`
  re-applies it with light sleep on for the sender. Keep
  `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` in `sdkconfig.defaults` in step with it.
  History: it was pinned at 80 MHz (commits `40baea3`/`fa24b5e`/`c53b028`); the
  device is mains-powered and the dashboard/backfill are CPU/SPIFFS bound, so the
  top clock buys responsiveness while a re-sync runs. If a board shows brownout
  resets at 240 MHz, lower both knobs to 160 or 80.
- **Generated `sdkconfig*` files are git-ignored** (only `sdkconfig.defaults` is
  tracked). `sdkconfig`, `sdkconfig.receiver`, `sdkconfig.sender` are local
  artefacts; an old local `sdkconfig.*` pins `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=80`
  and **overrides** `sdkconfig.defaults`, so after changing the ceiling re-run
  `idf.py reconfigure` (or delete the local `sdkconfig.*`) or the build keeps
  80 MHz. The CI matrix regenerates `sdkconfig` from `sdkconfig.defaults` + role
  defaults, so CI is unaffected by stale local files.
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
  four PM options and the 240 MHz ceiling.
- Wi-Fi power save stays at the default `WIFI_PS_MIN_MODEM`; `WIFI_PS_NONE`
  would burn power and power save beyond the default hurts ESP-NOW reliability.
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
  only reboot. A post-confirmation health watchdog is still open (see TODO.md).

## Watchdog (reviewed, not enabled)
- No `esp_task_wdt_*` usage anywhere; the task watchdog (TWDT) and interrupt
  watchdog are left at the IDF defaults (enabled, panic on timeout). So a hang
  in a watched task already reboots the device and shows up as
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
  restarts …`). `max_open_sockets` auf 10 erhöht (Headroom für die Probe).


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

### Separater Wunsch des Nutzers (NICHT dieser Fix)
„Nach dem CSV-Upload bleibt das Interface minutenlang offline." Der Handler
`power_history_upload_handler` rebootet **nicht**; er streamt den Body. Vermutete Ursache:
die große Upload-Transaktion blockiert den HTTPD-Task; zusätzlich belastet die
Dashboard-Auto-Aktualisierung (`updateHistory` alle 1,5 s) den Server. Eigener Task: Upload
in den Backfill-Worker verlagern bzw. HTTPD während des Imports nicht blockieren.

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
