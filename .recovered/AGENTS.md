# esp32-IR – repository notes for agents

ESP-IDF firmware for the ESP32-IR project (GitHub `christiansiebe/esp32-IR`).
Two firmware roles are built from one source tree.

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
- App builds with `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (-Os). The default
  `Debug` optimisation left the image just inside the 1 MB app partition; the
  MQTT bridge pushed it ~6 KB over, and -Os reclaimed ~90 KB. The flash layout
  is unchanged, so OTA stays compatible with already deployed devices.
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

## CPU clock / power
- CPU runs at a fixed 80 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80=y` in
  `sdkconfig.defaults`), not the IDF default 160 MHz. The device is I/O bound.
- Deliberately **not** dynamic frequency scaling (DFS / `CONFIG_PM_ENABLE`):
  the Wi-Fi driver holds an `ESP_PM_APB_FREQ_MAX` lock while interfaces are up
  (so DFS settles at 80 MHz anyway), and on the ESP32 DFS must be disabled for
  reliable ESP-NOW (connectionless) timing.
- Wi-Fi power save stays at the default `WIFI_PS_MIN_MODEM`; `WIFI_PS_NONE`
  would burn power and power save beyond the default hurts ESP-NOW reliability.

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

## Receiver health watchdog (enabled)
- The receiver runs `health_task.c` (started from `app_main`, receiver role
  only). It evaluates the pure rules in `health_policy.h` every 5 s and calls
  `esp_restart()` when the meter data has been silent for longer than
  `stale_data_ms` (15 min) or the free heap dropped below `low_heap_bytes`
  (12 KB), after a `grace_ms` (3 min) start-up window. A reason latches so the
  reboot decision does not flap. `last_rx_ms == 0` (no value yet, e.g. no sender
  in range during commissioning) is *not* treated as stale.
- The health task subscribes to the task watchdog (`esp_task_wdt_add`), so a
  wedged health loop also reboots. The sender does not run this task.
- The sender's workers all block on a queue/semaphore/UART read with a finite
  timeout and have no busy loop, so they should not trip the TWDT. `app_main`
  returns after `run_sender()`, and the IDF main task is subscribed to the TWDT
  by default; it does not idle-panic here (default `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1`).
- Consideration for a future change: enabling `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0`
  would catch a fully starved CPU0, but on the ESP32 the Arduino-ESP32 core
  disables the idle-task checks specifically to avoid false resets, and the
  project has no hardware-in-the-loop test to validate it. Left off deliberately.

## MQTT bridge (receiver, optional)
- `mqtt_bridge.c` holds the pure, host-tested helpers (config parse/validate,
  topic building, Home Assistant discovery + state JSON). `mqtt_publish.c` owns
  the ESP-IDF client and NVS persistence. The bridge is off by default; enable
  and configure it over the web console: `mqtt <host[:port]> [user] [pass]
  [base-topic]` or `mqtt off`. Config lives in the `mqttcfg` NVS namespace.
- TLS/WebSocket transports are disabled in `sdkconfig.defaults` because the
  default `espressif/mqtt` component pulls in mbedTLS and overflows the 1 MB app
  partition; the bridge targets a plain-TCP local broker. The app is also built
  with `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` for the same reason (see "Build and
  configuration").
- The bridge is best effort: a missing broker, connection failure or publish
  error never blocks `app_main` or the dashboard.

## Post-OTA rollback confirmation (receiver)
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and the two OTA slots + otadata are
  already in place, so a freshly flashed image starts as
  `ESP_OTA_IMG_PENDING_VERIFY`: *any* reset (including a task-watchdog panic)
  before it is confirmed rolls back to the previous image.
- The image is therefore **not** confirmed from `app_main`. `boot_confirm.c`
  (pattern from the IDF `native_ota_example`) records the pending state, notes
  the dashboard up once `httpd_start()` returned, and confirms after the
  stability window (45 s). If the dashboard never comes up within the boot grace
  (30 s), the running image is not started by the bootloader again:
  `esp_ota_mark_app_invalid_rollback_and_reboot()` falls back.
- Arrival of SML meter data is deliberately **not** a confirmation requirement:
  a silent/absent sender must never roll back an otherwise healthy image.
- The health watchdog calls `boot_confirm_fail()`: while the image is still
  unconfirmed it rolls back, otherwise it is a plain `esp_restart()`.
- The decision is pure and host-simulated in `boot_policy.c` /
  `test/test_boot_policy.c` (healthy -> confirm; no dashboard -> rollback;
  late dashboard -> rollback; confirmed image -> never rollback).

## Manual recovery button (receiver)
- The on-board BOOT button (`CONFIG_RECOVERY_BUTTON_GPIO`, default GPIO0) is the
  escape hatch for a *confirmed but broken* image: holding it for
  `CONFIG_RECOVERY_BUTTON_HOLD_MS` (default 3 s) at startup calls
  `esp_ota_mark_app_invalid_rollback_and_reboot()` directly. In IDF this is
  allowed even on a `VALID` image (see `esp_ota_current_ota_is_workable(false)`),
  provided a valid image exists in the other OTA slot; otherwise it returns
  `ESP_ERR_OTA_ROLLBACK_FAILED` and a normal boot continues.
- The button is a strapping pin (held at boot -> UART download mode), so it is
  sampled only after the application has started; the gesture logic lives in the
  pure, host-simulated `recovery_button.c` / `test/test_recovery_button.c`
  (hold 3 s -> rollback; short tap or two short taps -> nothing).
- Setting `CONFIG_RECOVERY_BUTTON_GPIO=-1` disables it. Requires the
  `esp_driver_gpio` component.

## Watchdog (sender, reviewed, not enabled)
- The sender has no `esp_task_wdt_*` usage; the task watchdog (TWDT) and interrupt
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
