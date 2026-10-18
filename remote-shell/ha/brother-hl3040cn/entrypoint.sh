#!/bin/sh
# Entrypoint: create (or refresh) the HL-3040CN queue and run CUPS in the
# foreground. Idempotent - safe to restart the container.
set -eu

PRINTER_IP="${PRINTER_IP:-192.168.178.50}"
PRINTER_PORT="${PRINTER_PORT:-9100}"
QUEUE="${QUEUE:-HL3040CN}"
PPD=/usr/share/ppd/Brother/brother_hl3040cn_printer_en.ppd
URI="socket://${PRINTER_IP}:${PRINTER_PORT}"

# Run cupsd in the foreground (as a child of this script) so the container
# lifetime follows the daemon, then create the queue against the live scheduler.
cupsd -f &
CUPSD_PID=$!

# Wait for the scheduler socket, then (re)create the queue.
i=0
while [ "$i" -lt 30 ]; do
    if lpstat -r >/dev/null 2>&1; then break; fi
    i=$((i + 1)); sleep 1
done

if lpstat -p "$QUEUE" >/dev/null 2>&1; then
    lpadmin -p "$QUEUE" -v "$URI"
else
    lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD"
fi
lpadmin -p "$QUEUE" -o pdftops-renderer-default=gs
cupsenable "$QUEUE"
cupsaccept "$QUEUE"

echo "queue '$QUEUE' -> $URI (filter: brlpdwrapperhl3040cn under qemu-i386)"

wait "$CUPSD_PID"
