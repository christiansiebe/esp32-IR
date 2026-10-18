#!/bin/sh
# Build and run the Brother HL-3040CN print server container on the Odroid.
#
# Why a container: the HL-3040CN is host-based (GDI) - it understands neither
# PCL6 nor PostScript, so generic drivers print blank pages. Brother ships the
# filter for i386 only (no ARM, no source), so it must run under qemu-i386
# emulation. This script makes sure binfmt is registered, builds the image and
# starts CUPS with the HL-3040CN queue on port 631.
#
# Aufruf:  ./deploy_brother_cups.sh [PRINTER_IP]
# Default: 192.168.178.50
set -eu

HERE=$(dirname "$(readlink -f "$0")")
PRINTER_IP="${1:-192.168.178.50}"
IMAGE=brother-hl3040cn:local
NAME=brother-cups

for f in Dockerfile entrypoint.sh cupsd.conf; do
    [ -f "$HERE/$f" ] || { echo "FEHLER: $HERE/$f fehlt" >&2; exit 1; }
done

echo "== 1. qemu-i386 binfmt sicherstellen =="
if [ ! -e /proc/sys/fs/binfmt_misc/qemu-i386 ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq qemu-user-static binfmt-support
fi
ls /proc/sys/fs/binfmt_misc/qemu-i386 >/dev/null 2>&1 \
    && echo "   qemu-i386 aktiv" \
    || { echo "FEHLER: qemu-i386 binfmt nicht aktiv" >&2; exit 1; }

echo "== 2. i386-Emulation testen =="
docker run --rm --platform linux/386 i386/debian:bookworm-slim uname -m

echo "== 3. Image bauen ($IMAGE) =="
docker build -t "$IMAGE" "$HERE"

echo "== 4. Container starten ($NAME) =="
docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" \
    --restart unless-stopped \
    -p 631:631 \
    -e "PRINTER_IP=$PRINTER_IP" \
    "$IMAGE" >/dev/null

sleep 6
echo "== 5. Zustand =="
docker ps --filter "name=$NAME" --format '{{.Names}} {{.Status}} {{.Ports}}'
echo "-- Queue --"
docker exec "$NAME" lpstat -v 2>/dev/null || echo "   (Queue noch nicht bereit)"
echo
echo "== fertig =="
echo "   CUPS:   http://$(hostname).local:631/printers/HL3040CN"
echo "   Test:   docker exec $NAME lp -d HL3040CN /usr/share/... (oder vom LAN drucken)"
echo "   Logs:   docker logs $NAME"
echo "   Stop:   docker rm -f $NAME"
