#!/usr/bin/env bash
# Build and run the host tests for the power-history aggregation helpers.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"

cc="${CC:-gcc}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$cc" -std=c11 -Wall -Wextra -O2 \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$here/test_power_history.c" \
    -lm -o "$out/test_power_history"

"$out/test_power_history"

"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/web_console.c" \
    "$here/test_web_console.c" \
    -o "$out/test_web_console"

"$out/test_web_console"

"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/crash_journal.c" \
    "$here/test_crash_journal.c" \
    -o "$out/test_crash_journal"

"$out/test_crash_journal"

# Reboot-persistence: drives the real init/write/status cycle against a scratch
# file and simulates a reboot with a fresh process. The mount point and history
# file are redirected into the temp build dir (no /powerlog on the host).
"$cc" -std=c11 -Wall -Wextra -O2 -pthread \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -DPOWER_HISTORY_MOUNT_PATH="\"$out/powerlog\"" \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$here/test_reboot_persistence.c" \
    -lm -o "$out/test_reboot_persistence"

"$out/test_reboot_persistence"

# Header-handling + migration: a store written by an older firmware (older
# header/record version) must not be treated as "empty" at boot, and the
# "Persistent history ready: N of ..." line must match the record_count the API
# reports. Each scenario boots in its own process against its own store file.
"$cc" -std=c11 -Wall -Wextra -O2 -pthread \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -DPOWER_HISTORY_MOUNT_PATH="\"$out/powerlog-hdr\"" \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$here/test_header_migration.c" \
    -lm -o "$out/test_header_migration"

"$out/test_header_migration"

# Post-OTA rollback decision (receiver). Pure, host-testable logic: replays boot
# scenarios tick by tick and asserts confirm / rollback / nothing. No ESP-IDF;
# the production glue (boot_confirm.c) that calls the OTA API is not part of the
# host build (hardware verification still open).
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/boot_policy.c" \
    "$here/test_boot_policy.c" \
    -o "$out/test_boot_policy"

"$out/test_boot_policy"

# The trigger half: the real boot_confirm.c glue (with test hooks) must actually
# perform the rollback for every failure scenario — the link that was missing.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -DBOOT_CONFIRM_TEST_HOOKS \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/boot_policy.c" \
    "$repo/main/boot_confirm.c" \
    "$here/test_boot_confirm_trigger.c" \
    -o "$out/test_boot_confirm_trigger"

"$out/test_boot_confirm_trigger"

# JSON string escaper: the sender-diagnostics JSON interpolates free-form event
# texts, so a stray control character must not break the payload the browser
# parses ("bad control character in string literal").
"$cc" -std=c11 -Wall -Wextra -O2 \
    -I"$repo/main" \
    "$repo/main/json_escape.c" \
    "$here/test_json_escape.c" \
    -o "$out/test_json_escape"

"$out/test_json_escape"

# Pure MQTT topic / Home Assistant discovery builders (no esp-mqtt): the string
# layout is what breaks silently, so it is host-tested like boot_policy.c.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/mqtt_topics.c" \
    "$here/test_mqtt_topics.c" \
    -o "$out/test_mqtt_topics"

"$out/test_mqtt_topics"

# Sender-side batching ring + backfill arithmetic (no radio): batching,
# oldest-eviction and the gap count the receiver reconciles against.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" -I"$repo/components/sml_transport/include" \
    "$repo/main/tx_batch.c" \
    "$here/test_tx_batch.c" \
    -o "$out/test_tx_batch"

"$out/test_tx_batch"

# Adaptive sender light-sleep window (main/uart_sleep_window.h). Pure C: the
# min-gap cap and the back-off must never let the window clip a telegram.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$here/test_uart_sleep_window.c" \
    -o "$out/test_uart_sleep_window"

"$out/test_uart_sleep_window"

# Timing simulation: the adaptive controller driven against a synthetic SML
# read head, modelling the APB-clock stop on light sleep (a telegram that
# arrives mid-sleep loses its leading bytes). Proves settling, the longer-sleep
# optimum, and that a sudden speed-up costs only a bounded number of telegrams.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$here/test_uart_sleep_sim.c" \
    -o "$out/test_uart_sleep_sim"

"$out/test_uart_sleep_sim"

# Failure question for the sleep controller: when the window is wrong, where
# does the loss end up? Drives the controller against a synthetic read head and
# shows that transient clipping never empties a minute (recoverable) while a
# persistently jammed head does (real loss, not repairable by backfill).
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$here/test_sleep_misbehavior.c" \
    -o "$out/test_sleep_misbehavior"

"$out/test_sleep_misbehavior"

# Link fallback: the peer-silence decision that returns a device to its proven
# mode. Pure C; the transition must be sticky (no flapping) and grace-bounded.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/link_fallback.c" \
    "$here/test_link_fallback.c" \
    -o "$out/test_link_fallback"

"$out/test_link_fallback"

# Receiver idle light sleep: the gate that parks the mains-powered receiver
# between dashboard requests. Pure C; the boot grace window and the idle
# transition must be exact or the CPU sleeps before Wi-Fi/HTTP are up.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$here/test_receiver_sleep.c" \
    -o "$out/test_receiver_sleep"

"$out/test_receiver_sleep"

# Receiver-side backfill merge bookkeeping (window/presence/dedupe). Pure C.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/history_backfill.c" \
    "$here/test_history_backfill.c" \
    -o "$out/test_history_backfill"

"$out/test_history_backfill"

# End-to-end sync session with a mock sender: full mirror, resume after a
# simulated link drop, and dedupe against the receiver's own data.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/history_sync.c" \
    "$repo/main/history_backfill.c" \
    "$here/test_history_sync.c" \
    -o "$out/test_history_sync"

"$out/test_history_sync"

# Stress simulation: a large sender ring pulled over many adverse sessions
# (random link drops + store pressure). Must converge to an exact mirror with no
# lost minute, no duplicate, and a cursor that never skips data.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/history_sync.c" \
    "$repo/main/history_backfill.c" \
    "$here/test_stress_history_sync.c" \
    -o "$out/test_stress_history_sync"

"$out/test_stress_history_sync"

# The store-side half of the backfill: power_history_merge() must validate and
# persist pulled records and reject corrupt ones. Same plain-file fallback as
# the reboot-persistence test (scratch mount under the temp build dir).
"$cc" -std=c11 -Wall -Wextra -O2 -pthread \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -DPOWER_HISTORY_MOUNT_PATH="\"$out/powerlog-merge\"" \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$here/test_history_backfill_merge.c" \
    -lm -o "$out/test_history_backfill_merge"

"$out/test_history_backfill_merge"

# Boot-marker exchange: proves a marker minute persists with its reason, crosses
# the backfill merge intact and idempotently, and that the window summary counts
# only minutes with no record at all as real loss.
"$cc" -std=c11 -Wall -Wextra -O2 -pthread \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -DPOWER_HISTORY_MOUNT_PATH="\"$out/powerlog-marker\"" \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$here/test_boot_marker_exchange.c" \
    -lm -o "$out/test_boot_marker_exchange"

"$out/test_boot_marker_exchange"

# CSV restore parser (receiver "restore from backup" upload): must accept exactly
# what /api/power-history.csv exports and reject what it cannot trust. Pure C.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$repo/main/history_csv_import.c" \
    "$here/test_history_csv_import.c" \
    -lm -o "$out/test_history_csv_import"

"$out/test_history_csv_import"

# The store-side half of the CSV restore: power_history_import_sorted() appends
# a backup, dedupes existing minutes (keeping live values) and stays readable.
"$cc" -std=c11 -Wall -Wextra -O2 -pthread \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -DPOWER_HISTORY_MOUNT_PATH="\"$out/powerlog-csv\"" \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$here/test_history_csv_import_store.c" \
    -lm -o "$out/test_history_csv_import_store"

"$out/test_history_csv_import_store"

# Ring repair + backup upload: an already-scrambled store (older firmware left
# the ring out of order) is repaired at init, and a CSV-restore upload inserts
# the missing minutes in time order without duplicating live ones.
"$cc" -std=c11 -Wall -Wextra -O2 -pthread \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -DPOWER_HISTORY_MOUNT_PATH="\"$out/powerlog-repair\"" \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$here/test_history_ring_repair.c" \
    -lm -o "$out/test_history_ring_repair"

"$out/test_history_ring_repair"
# Full ring (29000) unsorted + duplicates -> forces the in-place disk sort.
"$out/test_history_ring_repair" full

python3 "$here/test_ha_stats.py"
python3 "$here/test_ws_min.py"
python3 "$here/test_ws_client.py"
python3 "$here/check_dashboard_js.py"

# cpu-real-clock: the top-OPP detection that makes scaling_cur_freq report the
# real (possibly overclocked) PLL clock instead of the driver's cached OPP.
python3 "$repo/remote-shell/ha/test_cpu_real_clock.py"

# Brother HL-3040CN print server container (Odroid): the filter chain that makes
# a host-based GDI printer work on ARM via qemu-i386 - pins the config so an
# edit cannot silently drop a filter helper or the PPD.
python3 "$repo/remote-shell/ha/test_brother_cups.py"

# Fleet supervisor decision table (pure, no Cloud API): the cases that let an
# agent fall asleep and never wake (shared parent sandbox, a failed turn, a
# finished worker sharing a live sandbox).
python3 "$repo/fleet/test_supervisor.py"

# Firmware version: must always resolve to a non-empty string, so the dashboard
# and the diagnostics can identify the running build (CMake writes
# main/build_version.h; host builds fall back to PROJECT_VER / "unknown").
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/version.c" \
    "$here/test_version.c" \
    -o "$out/test_version"

"$out/test_version"

# Web-health guard policy: restart-on-silence escalation and, crucially, that
# the guard stays passive until the self-probe has proven itself once (a build
# without the loopback path must not reboot a healthy device).
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/web_guard.c" \
    "$here/test_web_guard.c" \
    -o "$out/test_web_guard"

"$out/test_web_guard"

# Global liveness watchdog: the staged escalation that reboots a device whose
# workers have all stalled (the "refuses service" backstop).
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/liveness_guard.c" \
    "$here/test_liveness_guard.c" \
    -o "$out/test_liveness_guard"

"$out/test_liveness_guard"

# OTA safety policy: the update slot must never overlap the storage partition,
# and a pull-OTA URL must be https. Pure, driven with the real partitions.csv
# addresses so the guard that protects the measurement history is covered.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/ota_policy.c" \
    "$here/test_ota_policy.c" \
    -o "$out/test_ota_policy"

"$out/test_ota_policy"

# Runtime test knobs behind the console's `set` / `reset-config`: clamping and
# rejection must be exact so a bench run never silently changes the CPU ceiling
# or the light-sleep flag. Pure C.
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/main" \
    "$repo/main/runtime_config.c" \
    "$here/test_runtime_config.c" \
    -o "$out/test_runtime_config"

"$out/test_runtime_config"

# Parser robustness runs under AddressSanitizer so buffer overruns in the
# vendored SML parser fail the build instead of corrupting the sender at runtime.
cxx="${CXX:-g++}"
"$cxx" -std=c++17 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/components/sml_parser/src" \
    -I"$repo/components/sml_parser/include" \
    -I"$repo/components/sml_transport/include" \
    "$repo/components/sml_parser/src/sml.cpp" \
    "$repo/components/sml_parser/src/sml_adapter.cpp" \
    "$here/test_sml_parser.cpp" \
    -o "$out/test_sml_parser"

ASAN_OPTIONS=detect_leaks=1 "$out/test_sml_parser"

# Golden decode: a real eHZ telegram must be decoded to its exact OBIS values,
# and a flipped byte must be rejected as a checksum error. Locks the parser's
# byte-level output against silent regressions.
"$cxx" -std=c++17 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/components/sml_parser/src" \
    -I"$repo/test" \
    "$repo/components/sml_parser/src/sml.cpp" \
    "$here/test_sml_parser_golden.cpp" \
    -o "$out/test_sml_parser_golden"

ASAN_OPTIONS=detect_leaks=0 "$out/test_sml_parser_golden"

# The console's `test sml` self-test must reach PASS on a healthy parser; this
# locks the device-facing verdict (same fixture and values) as well.
"$cxx" -std=c++17 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/components/sml_parser/src" \
    -I"$repo/components/sml_parser/include" \
    -I"$repo/components/sml_transport/include" \
    "$repo/components/sml_parser/src/sml.cpp" \
    "$repo/components/sml_parser/src/sml_selftest.cpp" \
    "$here/test_sml_selftest.cpp" \
    -o "$out/test_sml_selftest"

ASAN_OPTIONS=detect_leaks=0 "$out/test_sml_selftest"

# Stability / fault-injection harness for the power-history logic (ASan+UBSan):
# corrupted, truncated, adversarial and boundary inputs must never crash or be
# accepted as consistent records.
"$here/stability/run_stability.sh"

# Virtual link simulator: drives the real history_sync_run() against a scripted
# sender and a lossy ESP-NOW link, so backfill changes can be validated on the
# host before flashing. Asserts a clean mirror, a resume after a link drop with
# no lost/duplicated minute, and caught_up against the sender's real horizon
# (the regression where a synthetic latest_utc pinned the receiver to a 10 s
# poll).
"$cc" -std=c11 -Wall -Wextra -O2 \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$here/host/include" \
    -I"$repo/main" \
    "$repo/main/history_sync.c" \
    "$repo/main/history_backfill.c" \
    "$here/test_virtual_day.c" \
    -o "$out/test_virtual_day"

"$out/test_virtual_day"
