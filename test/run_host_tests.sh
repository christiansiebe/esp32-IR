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

python3 "$here/test_ha_stats.py"
python3 "$here/test_ws_min.py"
python3 "$here/test_ws_client.py"
python3 "$here/check_dashboard_js.py"

# Parser robustness runs under AddressSanitizer so buffer overruns in the
# vendored SML parser fail the build instead of corrupting the sender at runtime.
cxx="${CXX:-g++}"
"$cxx" -std=c++17 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/components/sml_parser/src" \
    "$repo/components/sml_parser/src/sml.cpp" \
    "$here/test_sml_parser.cpp" \
    -o "$out/test_sml_parser"

ASAN_OPTIONS=detect_leaks=1 "$out/test_sml_parser"
