# esp32-IR – repository notes for agents

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
  value transport. No web server.
- **Receiver**: ESP-NOW value transport, SPIFFS power history, and the Wi-Fi
  dashboard (AP `ESP32-IR-Meter` + optional STA to the FRITZ!Box).
- Role selection is a Kconfig `choice` in `main/Kconfig.projbuild`
  (`CONFIG_ESPNOW_ROLE_SENDER` / `_RECEIVER`).
- **Non-negotiable / must-not-break**: ESP-NOW, OTA, roles.
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
  factory+2×1 MB OTA layout, giving each image +512 KB headroom.
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
  `storage.bin` **only on the very first deployment** — afterwards leaving it out
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
- CPU is capped at 80 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80=y` in
  `sdkconfig.defaults`), not the IDF default 160 MHz, and the clock is
  **variable**: `CONFIG_PM_ENABLE=y` + `CONFIG_PM_DFS_INIT_AUTO=y` scale it down
  to the 40 MHz XTAL when idle, and `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` adds
  automatic light sleep between cycles (with `CONFIG_PM_SLP_IRAM_OPT`,
  `CONFIG_PM_RTOS_IDLE_OPT`, `CONFIG_ESP_WIFI_SLP_IRAM_OPT` to cut wake cost,
  and `CONFIG_PM_LIGHT_SLEEP_CALLBACKS=y` so the firmware can report whether it
  actually slept). The 80 MHz ceiling is kept for the ESP-NOW send burst.
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
  80 MHz behaviour; the CI job "Verify the variable CPU clock is in effect"
  checks the four PM options and the 80 MHz ceiling.
- Wi-Fi power save stays at the default `WIFI_PS_MIN_MODEM`; `WIFI_PS_NONE`
  would burn power and power save beyond the default hurts ESP-NOW reliability.
- Adaptive sender light sleep (`CONFIG_ESPNOW_SENDER_LIGHT_SLEEP`, default
  **off**): `main/uart_sleep_window.{h,c}` parks the CPU only in the quiet gap
  between SML telegram bursts. The window starts at 400 ms, is capped below the
  shortest inter-telegram gap seen (3/4 margin), grows 25 ms per clean sleep and
  halves on an overrun. An `ESP_PM_NO_LIGHT_SLEEP` lock is held for the whole
  burst so the APB clock never stops mid-telegram (there is no UART wake-up
  source). `uart_sleep_hold()` from the ESP-NOW receive path keeps the CPU awake
  for an in-flight history pull (2 s) or OTA (5 s). Host tests:
  `test/test_uart_sleep_window.c` and `test/test_uart_sleep_sim.c` (the sim
  models the lost bytes when a telegram arrives mid-sleep). CI syntax-checks the
  enabled branch with IDF stubs in `test/host/include`. Bench verification of the
  enabled path is still open (see `TODO.md`).

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
  unconfirmed at ~1 telegram/s ≈ 4–5 min) evicts the oldest and that value shows
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

## Managed components (IDF v6)
- ESP-IDF v6 moved ESP-MQTT and cJSON out of the core. They come from the ESP
  Component Registry (`espressif/mqtt`, `espressif/cjson`) via
  `main/idf_component.yml`; their component names in the build are
  `espressif__mqtt` / `espressif__cjson`, and the manifest makes them a
  dependency of `main` automatically - naming them in `REQUIRES` is not needed.
- The IDF build compiles with `-Werror`: unused vars and `-Wformat-truncation`
  (e.g. a `snprintf` whose buffer is smaller than the topic could be) fail CI.
  Size buffers for the full topic length.
