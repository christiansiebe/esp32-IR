#!/usr/bin/env bash
# ota-publish.sh — let the sandbox serve a firmware image so a device can pull it.
#
# The sandbox has no route into the LAN (DS-Lite), so it can never open a
# connection to the ESP32. It *can* serve a file publicly (port 12000 ->
# https://work-1-<id>.prod-runtime.all-hands.dev/), and the device can pull it
# over HTTPS. This script makes the built .bin reachable and prints the exact
# command that triggers the update on the device.
#
# Usage:
#   tools/ota-publish.sh build/receiver/esp32-IR.bin
#   tools/ota-publish.sh build/sender/esp32-IR.bin 192.168.178.71
#
# The trigger must run on a host that can reach the device (the Odroid, or your
# PC): the sandbox only hosts the image. With the command channel up you can run
# it straight on the Odroid:
#   bash /workspace/remote-shell/cmd_send.sh "<the printed curl>"
set -euo pipefail

BIN="${1:?Usage: ota-publish.sh <firmware.bin> [device-ip]}"
DEVICE_IP="${2:-192.168.178.71}"
UPLOAD_DIR="${UPLOAD_DIR:-/workspace/uploads}"
PORT="${OTA_PUBLISH_PORT:-12000}"
RUNTIME_ID="${RUNTIME_ID:?RUNTIME_ID is not set}"
PUBLIC_URL="https://work-1-${RUNTIME_ID}.prod-runtime.all-hands.dev/files"

[ -f "$BIN" ] || { echo "FEHLER: $BIN nicht gefunden"; exit 1; }
[ -s "$BIN" ] || { echo "FEHLER: $BIN ist leer"; exit 1; }

# The upload server both accepts files and serves them back at /files/<name>.
if ! (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then
    echo "upload-server laeuft nicht, starte ihn ..."
    mkdir -p "$UPLOAD_DIR"
    nohup python3 /workspace/upload-server/server.py >/tmp/ota-publish-server.log 2>&1 &
    sleep 2
fi

NAME="$(basename "$BIN")"
cp -f "$BIN" "$UPLOAD_DIR/$NAME"
URL="$PUBLIC_URL/$NAME"

echo "bereitgestellt: $URL"
if command -v curl >/dev/null 2>&1; then
    CODE=$(curl -s -m 10 -o /dev/null -w '%{http_code}' "$URL" || true)
    echo "oeffentlich erreichbar: HTTP $CODE"
    [ "$CODE" = "200" ] || { echo "WARNUNG: URL liefert $CODE - Firewall/Port pruefen"; }
fi

echo
echo "=== Update auf dem Geraet ausloesen (auf einem LAN-Host ausfuehren) ==="
echo "curl -sS -X POST 'http://${DEVICE_IP}/api/ota-pull' \\"
echo "     -H 'X-OTA-Key: <WLAN-/OTA-Key>' \\"
echo "     -H 'Content-Type: application/json' \\"
echo "     -d '{\"url\":\"${URL}\"}'"
echo
echo "Das Geraet laedt das Image per HTTPS, prueft es, schreibt es in den"
echo "Update-Slot (die storage-Messdaten-Partition bleibt unberuehrt) und startet neu."
echo "Bei laufendem Kommando-Kanal direkt auf dem Odroid:"
echo "  bash /workspace/remote-shell/cmd_send.sh \"curl -sS -X POST 'http://${DEVICE_IP}/api/ota-pull' -H 'X-OTA-Key: <KEY>' -H 'Content-Type: application/json' -d '{\\\"url\\\":\\\"${URL}\\\"}'\" 120"
