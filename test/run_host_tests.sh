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

python3 "$here/test_ha_stats.py"
python3 "$here/test_ws_min.py"
python3 "$here/test_ws_client.py"
