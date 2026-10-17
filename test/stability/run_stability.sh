#!/usr/bin/env bash
# Stability / fault-injection tests for the power-history logic, built with
# AddressSanitizer + UndefinedBehaviorSanitizer so memory bugs fail loudly.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

cc="${CC:-gcc}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

"$cc" -std=c11 -Wall -Wextra -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -D_GNU_SOURCE -D_DEFAULT_SOURCE \
    -Wno-unused-variable -Wno-unused-const-variable \
    -I"$repo/test/host/include" \
    -I"$repo/main" \
    "$repo/main/power_history.c" \
    "$here/test_stability_power_history.c" \
    -lm -o "$out/test_stability"

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/test_stability"
