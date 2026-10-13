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

python3 "$here/test_ha_stats.py"
python3 "$here/test_ws_min.py"
python3 "$here/test_ws_client.py"
python3 "$here/check_dashboard_js.py"

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

# Stability / fault-injection harness for the power-history logic (ASan+UBSan):
# corrupted, truncated, adversarial and boundary inputs must never crash or be
# accepted as consistent records.
"$here/stability/run_stability.sh"
