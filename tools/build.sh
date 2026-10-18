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

# Locate a working docker command (the socket is often root-only).
docker_cmd="docker"
if ! docker info >/dev/null 2>&1; then
    if command -v sudo >/dev/null 2>&1 && sudo docker info >/dev/null 2>&1; then
        docker_cmd="sudo docker"
    else
        echo "Docker daemon is not reachable. Start it (e.g. 'sudo dockerd &') first." >&2
        exit 1
    fi
fi
# shellcheck disable=SC2086
$docker_cmd version --format '{{.Server.Version}}' >/dev/null

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

# Wipe inside the container: it runs as root, so this works even though build/
# may be owned by root from a previous containerised build.
if [[ "$clean" == "--clean" ]]; then
    # shellcheck disable=SC2086
    $docker_cmd run --rm -v "$repo":/project -w /project espressif/idf:v6.1 \
        rm -rf "build/$role" "sdkconfig.$role"
fi

mkdir -p "$repo/.ccache"
# -DSDKCONFIG (a CMake cache variable) is required: an exported SDKCONFIG env
# var is ignored by the idf.py CLI, so both roles would otherwise share the
# project ./sdkconfig and the second build would silently use the wrong role.
# shellcheck disable=SC2086
$docker_cmd run --rm \
    -v "$repo":/project \
    -v "$repo/.ccache":/root/.cache/ccache \
    -v "$gen":/tmp/extra.defaults:ro \
    -e IDF_CCACHE_ENABLE=1 -e CCACHE_DIR=/root/.cache/ccache \
    -e SDKCONFIG_DEFAULTS="sdkconfig.defaults;/tmp/extra.defaults" \
    -e IDF_TARGET=esp32 \
    -e FIRMWARE_VERSION="${FIRMWARE_VERSION:-}" \
    -w /project \
    espressif/idf:v6.1 \
    idf.py -B "build/$role" -DSDKCONFIG="/project/sdkconfig.$role" build

# Guard against a silent role mix-up.
if ! grep -q "CONFIG_ESPNOW_ROLE_${role_upper}=y" "$repo/sdkconfig.$role"; then
    echo "ERROR: sdkconfig.$role does not select the $role role" >&2
    exit 1
fi

echo "--- artifact ---"
ls -l "$repo/build/$role/esp32-IR.bin"
rm -f "$gen"
