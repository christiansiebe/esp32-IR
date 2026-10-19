#!/bin/sh
# odroid_ap_deadman.sh - auto-revert the AP config if it was never confirmed.
#
# Installed and armed by deploy_ap.sh, fired by odroid-ap-deadman.timer after N
# minutes. Semantics (same idea as oc_test_hold.sh):
#
#   marker present -> nobody confirmed -> run revert_ap.sh --apply (the AP
#                     config is rolled back and hostapd/dnsmasq go MASKED again)
#   marker absent  -> someone ran `deploy_ap.sh --confirm` -> do nothing
#
# The marker is /etc/odroid-ap.deadman. `deploy_ap.sh --confirm` deletes it and
# stops the timer, so a confirmed AP is never reverted.
#
# AP_ROOT is honoured for host-side testing (path prefix for the board tree).
set -u

HERE=$(dirname "$(readlink -f "$0")")
ROOT=${AP_ROOT:-}

MARKER=${ROOT}/etc/odroid-ap.deadman
REVERT_SH=${ROOT}/usr/local/sbin/odroid-ap-revert.sh
LOG=${ROOT}/var/log/odroid-ap-deadman.log

log() {
        if [ -n "$ROOT" ]; then
                mkdir -p "$(dirname "$LOG")"
                echo "$(date -u +%FT%TZ) $*" >> "$LOG"
        fi
        logger -t odroid-ap-deadman "$*" 2>/dev/null || true
        echo "odroid-ap-deadman: $*"
}

if [ -f "$MARKER" ]; then
        log 'kein Confirm - AP-Konfiguration wird zurueckgenommen'
        if [ -f "$REVERT_SH" ]; then
                AP_ROOT="$ROOT" sh "$REVERT_SH" --apply || \
                        log 'FEHLER: revert_ap.sh fehlgeschlagen'
        else
                log "FEHLER: $REVERT_SH fehlt - kann nicht zuruecknehmen"
        fi
        rm -f "$MARKER"
else
        log 'bestatigt (kein Marker) - nichts zu tun'
fi
exit 0
