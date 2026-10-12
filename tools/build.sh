#!/usr/bin/env bash
# Build the firmware in the official ESP-IDF container, so the host only needs
# Docker. Usage: tools/build.sh receiver|sender [--clean]
#
# The repo is mounted read/write so build/ ends up next to the sources and can
# be reused (ccache is shared in .ccache/). Safe to re-run; work is incremental.
set -euo pipefail

role="${1:-}"
if [[ "$role" != "receiver" && "$role" != "sender" ]]; then
    echo "usage: $0 receiver|sender [--clean]" >&2
    exit 2
fi
clean="${2:-}"

repo="$(cd "$(dirname "$0")/.." && pwd)"
role_upper="$(printf '%s' "$role" | tr '[:lower:]' '[:upper:]')"

if ! docker info >/dev/null 2>&1; then
    echo "Docker daemon is not running. Start it (e.g. 'sudo dockerd &') first." >&2
    exit 1
fi

# Per-role generated defaults, merged with the repo's sdkconfig.defaults.
gen="$(mktemp)"
cat > "$gen" <<EOF
CONFIG_IDF_TARGET="esp32"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPNOW_ROLE_${role_upper}=y
EOF

[[ "$clean" == "--clean" ]] && rm -rf "$repo/build/$role" "$repo/sdkconfig.$role"

mkdir -p "$repo/.ccache"
docker run --rm \
    -v "$repo":/project \
    -v "$repo/.ccache":/root/.cache/ccache \
    -v "$gen":/tmp/extra.defaults:ro \
    -e IDF_CCACHE_ENABLE=1 -e CCACHE_DIR=/root/.cache/ccache \
    -e SDKCONFIG="/project/sdkconfig.$role" \
    -e SDKCONFIG_DEFAULTS="sdkconfig.defaults;/tmp/extra.defaults" \
    -e IDF_TARGET=esp32 \
    -w /project \
    espressif/idf:v6.1 \
    idf.py -B "build/$role" build

echo "--- artifact ---"
ls -l "$repo/build/$role/esp32-IR.bin"
rm -f "$gen"
