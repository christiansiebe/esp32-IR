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
- **Non-negotiable / must-not-break**: ESP-NOW, OTA, partition layout, roles.

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
