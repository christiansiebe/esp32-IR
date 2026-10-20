#!/bin/sh
# diagnose_print.sh - find out why the Brother HL-3040CN print server prints
# blank pages (or looks like it spins).
#
# Run this ON THE ODROID (it needs docker and the CUPS container). It is
# read-only: it inspects the container, the binfmt registration and the printer
# reachability, then prints a verdict and the concrete fix.
#
#   ./diagnose_print.sh                 # default printer 192.168.178.50
#   ./diagnose_print.sh 192.168.178.50  # explicit
set -eu

HERE=$(dirname "$(readlink -f "$0")")
PRINTER_IP="${1:-192.168.178.50}"
NAME="${NAME:-brother-cups}"
QUEUE="${QUEUE:-HL3040CN}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "== HL-3040CN print-server diagnosis (printer $PRINTER_IP) =="

# --- container ---
RUNNING=false
RESTARTS=0
if docker inspect -f '{{.State.Running}}' "$NAME" >/dev/null 2>&1; then
    RUNNING=$(docker inspect -f '{{.State.Running}}' "$NAME")
    RESTARTS=$(docker inspect -f '{{.RestartCount}}' "$NAME")
fi
echo "container running:  $RUNNING (restarts: $RESTARTS)"

# --- binfmt (qemu-i386) ---
BINFMT=false
if [ -f /proc/sys/fs/binfmt_misc/qemu-i386 ]; then
    if grep -q '^enabled' /proc/sys/fs/binfmt_misc/qemu-i386; then BINFMT=true; fi
fi
echo "qemu-i386 binfmt:   $BINFMT"

# --- inside the container: filter, PPD, queue, CUPS log ---
FILTER="?"
PPD="?"
QSTATE=""
ACCEPTING="?"
CUPSLOG=""
if [ "$RUNNING" = "true" ]; then
    FILTER=$(docker exec "$NAME" sh -c \
        'test -x /usr/lib/cups/filter/brlpdwrapperhl3040cn && echo true || echo false' \
        2>/dev/null || echo "?")
    PPD=$(docker exec "$NAME" sh -c \
        'test -f /usr/share/ppd/Brother/brother_hl3040cn_printer_en.ppd && echo true || echo false' \
        2>/dev/null || echo "?")
    QSTATE=$(docker exec "$NAME" lpstat -p "$QUEUE" 2>/dev/null \
        | sed -n 's/.*printer .* is \([a-z]*\).*/\1/p' | head -1 || true)
    if docker exec "$NAME" lpstat -a "$QUEUE" 2>/dev/null | grep -qi accepting; then
        ACCEPTING=true
    else
        ACCEPTING=false
    fi
    CUPSLOG=$(docker exec "$NAME" sh -c \
        'tail -n 120 /var/log/cups/error_log 2>/dev/null' || true)
fi
echo "filter present:     $FILTER"
echo "ppd present:        $PPD"
echo "queue state:        ${QSTATE:-unknown}  accepting=${ACCEPTING}"

# --- printer reachability (TCP 9100) ---
REACH=false
if timeout 3 sh -c "exec 3<>/dev/tcp/$PRINTER_IP/9100" 2>/dev/null; then
    REACH=true
fi
echo "printer reachable:  $REACH ($PRINTER_IP:9100)"

# --- hand the signals to the pure classifier ---
export SIG_RUNNING="$RUNNING" SIG_RESTARTS="$RESTARTS" SIG_BINFMT="$BINFMT" \
       SIG_FILTER="$FILTER" SIG_PPD="$PPD" SIG_QSTATE="$QSTATE" \
       SIG_ACCEPTING="$ACCEPTING" SIG_REACH="$REACH"
printf '%s' "$CUPSLOG" > "$OUT/cups.log"

python3 - "$HERE" "$OUT/cups.log" <<'PY'
import json, os, sys
sys.path.insert(0, sys.argv[1])
from print_health import classify

def b(name):
    v = os.environ.get(name, "")
    if v == "true":  return True
    if v == "false": return False
    return None

sig = {
    "container_running": b("SIG_RUNNING"),
    "container_restarts": int(os.environ.get("SIG_RESTARTS") or 0),
    "binfmt_ok": b("SIG_BINFMT"),
    "filter_present": b("SIG_FILTER"),
    "ppd_present": b("SIG_PPD"),
    "queue_state": os.environ.get("SIG_QSTATE") or "",
    "accepting": b("SIG_ACCEPTING"),
    "printer_reachable": b("SIG_REACH"),
    "cups_log": open(sys.argv[2]).read() if os.path.exists(sys.argv[2]) else "",
}
r = classify(sig)
print()
print("VERDICT: " + r["verdict"])
print("CAUSE:   " + r["cause"])
print("FIX:")
for f in r["fixes"]:
    print("  - " + f)
PY
