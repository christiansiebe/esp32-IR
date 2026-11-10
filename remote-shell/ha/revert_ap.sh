#!/bin/sh
# revert_ap.sh - undo everything deploy_ap.sh installed. Idempotent.
#
# Restores the state before the deploy:
#   * stop + disable + MASK hostapd and dnsmasq (they were masked before)
#   * remove the NAT/forwarding rules deploy_ap.sh added
#   * hand the radio back to NetworkManager (managed yes)
#   * drop the AP gateway address from the interface
#   * remove the dnsmasq drop-in and restore /etc/default/hostapd
#   * disarm the dead-man switch (timer/service/marker/scripts)
#   * remove the state file
#
# Running it twice is fine: every step tolerates "already gone".
#
# Aufruf: revert_ap.sh [--apply] [--root PREFIX]
#   AP_ROOT  Pfad-Praefix fuer alle Board-Pfade (Tests/Dry-Run).
# Dry-run is the default (nothing changes); pass --apply to really revert.
set -u

HERE=$(dirname "$(readlink -f "$0")")

APPLY=0
ROOT_OPT=

usage() {
        cat <<'EOF'
Aufruf: revert_ap.sh [--apply] [--root PREFIX]

  (ohne --apply)  Dry-Run: zeigt nur, was passieren wuerde. Aendert NICHTS.
  --apply         wirklich zuruecknehmen (root noetig, ausser AP_ROOT gesetzt).

Umgebung:
  AP_ROOT         Pfad-Praefix fuer alle Board-Pfade (fuer Tests/Dry-Run).

Idempotent. Stellt den Zustand vor deploy_ap.sh wieder her
(hostapd/dnsmasq wieder MASKED).
EOF
}

while [ $# -gt 0 ]; do
        case "$1" in
                --apply) APPLY=1 ;;
                --dry-run) APPLY=0 ;;
                --root) shift; ROOT_OPT=${1:-} ;;
                -h|--help) usage; exit 0 ;;
                *) echo "unbekannte Option: $1" >&2; usage >&2; exit 2 ;;
        esac
        shift
done

ROOT=${ROOT_OPT:-${AP_ROOT:-}}
DRY=$((1 - APPLY))

HOSTAPD_CONF=${ROOT}/etc/hostapd/hostapd.conf
DNSMASQ_CONF=${ROOT}/etc/dnsmasq.d/odroid-ap.conf
NAT_SH=${ROOT}/usr/local/sbin/odroid-ap-nat.sh
REVERT_SH=${ROOT}/usr/local/sbin/odroid-ap-revert.sh
DEADMAN_SH=${ROOT}/usr/local/sbin/odroid_ap_deadman.sh
DEADMAN_SVC=${ROOT}/etc/systemd/system/odroid-ap-deadman.service
DEADMAN_TIMER=${ROOT}/etc/systemd/system/odroid-ap-deadman.timer
HOSTAPD_DEF=${ROOT}/etc/default/hostapd
HOSTAPD_DEF_BAK=${ROOT}/etc/default/hostapd.odroid-ap.bak
MARKER=${ROOT}/etc/odroid-ap.deadman
STATE=${ROOT}/etc/odroid-ap.state
CMDS=${ROOT}/var/log/odroid-ap-commands.log

say() { echo "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "   + $*"; else "$@"; fi; }
sys() {
        if [ "$DRY" = 1 ]; then
                echo "   + $*"
        elif [ -n "$ROOT" ]; then
                mkdir -p "$(dirname "$CMDS")"
                printf '%s\n' "$*" >> "$CMDS"
        else
                "$@"
        fi
}

[ "$DRY" = 1 ] || [ -n "$ROOT" ] || [ "$(id -u)" = 0 ] \
        || { echo "FEHLER: muss als root laufen (oder ohne --apply als Dry-Run)." >&2; exit 1; }

# Read back what was deployed, so the revert uses the same names.
AP_IFACE=wlxdc627949ba79
UP_IFACE=end0
AP_ADDR=192.168.50.1/24
if [ -f "$STATE" ] && [ "$DRY" != 1 ]; then
        # shellcheck disable=SC1090
        . "$STATE"
fi

say '== Odroid-AP zuruecknehmen =='
[ "$DRY" = 1 ] && say '   (dry-run: es wird nichts geaendert)'

say '== 1. Dead-Man-Switch entschaerfen =='
sys systemctl stop odroid-ap-deadman.timer
sys systemctl disable odroid-ap-deadman.timer
run rm -f "$MARKER"

say '== 2. hostapd/dnsmasq stoppen, deaktivieren, wieder maskieren =='
sys systemctl disable --now hostapd dnsmasq
sys systemctl mask hostapd dnsmasq

say '== 3. NAT/Forwarding-Regeln entfernen =='
# Deleting a rule that is not there fails; ignore the error.
if [ "$DRY" = 1 ]; then
        echo "   + iptables -t nat -D POSTROUTING -o $UP_IFACE -j MASQUERADE"
        echo "   + iptables -D FORWARD -i $AP_IFACE -o $UP_IFACE -j ACCEPT"
        echo "   + iptables -D FORWARD -i $UP_IFACE -o $AP_IFACE -m state --state RELATED,ESTABLISHED -j ACCEPT"
elif [ -n "$ROOT" ]; then
        mkdir -p "$(dirname "$CMDS")"
        printf '%s\n' \
          "iptables -t nat -D POSTROUTING -o $UP_IFACE -j MASQUERADE" \
          "iptables -D FORWARD -i $AP_IFACE -o $UP_IFACE -j ACCEPT" \
          "iptables -D FORWARD -i $UP_IFACE -o $AP_IFACE -m state --state RELATED,ESTABLISHED -j ACCEPT" \
          >> "$CMDS"
else
        iptables -t nat -D POSTROUTING -o "$UP_IFACE" -j MASQUERADE 2>/dev/null || true
        iptables -D FORWARD -i "$AP_IFACE" -o "$UP_IFACE" -j ACCEPT 2>/dev/null || true
        iptables -D FORWARD -i "$UP_IFACE" -o "$AP_IFACE" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
fi

say '== 4. Funkmodul an NetworkManager zurueckgeben =='
sys nmcli device set "$AP_IFACE" managed yes

say '== 5. AP-Adresse vom Interface entfernen =='
sys ip addr del "$AP_ADDR" dev "$AP_IFACE"

say '== 6. Dateien entfernen / /etc/default/hostapd wiederherstellen =='
run rm -f "$DNSMASQ_CONF"
if [ "$DRY" = 1 ]; then
        echo "   + rm -f $HOSTAPD_CONF"
        echo "   + /etc/default/hostapd aus $HOSTAPD_DEF_BAK wiederherstellen (falls Backup da), sonst DAEMON_CONF-Zeile entfernen"
else
        rm -f "$HOSTAPD_CONF"
        if [ -f "$HOSTAPD_DEF_BAK" ]; then
                mv -f "$HOSTAPD_DEF_BAK" "$HOSTAPD_DEF"
        elif [ -f "$HOSTAPD_DEF" ]; then
                sed -i '\|^DAEMON_CONF="/etc/hostapd/hostapd.conf"$|d' "$HOSTAPD_DEF"
        fi
fi
run rm -f "$DEADMAN_SVC" "$DEADMAN_TIMER" "$DEADMAN_SH" "$NAT_SH" "$REVERT_SH"
sys systemctl daemon-reload
sys systemctl reset-failed

say '== 7. Zustand loeschen =='
run rm -f "$STATE"

say '== 8. Kontrolle =='
if [ "$DRY" = 1 ]; then
        echo "   + systemctl is-enabled hostapd dnsmasq / is-active odroid-ap-deadman.timer"
else
        if [ -z "$ROOT" ]; then
                say "   hostapd: $(systemctl is-enabled hostapd 2>/dev/null || echo 'masked/unbekannt')"
                say "   dnsmasq: $(systemctl is-enabled dnsmasq 2>/dev/null || echo 'masked/unbekannt')"
        fi
        say "   AP-Adresse: $(ip addr show "$AP_IFACE" 2>/dev/null | grep -c "$AP_ADDR" || true)"
fi

say ''
if [ "$DRY" = 1 ]; then
        say '== dry-run fertig (nichts geaendert) =='
        say 'Wirklich zuruecknehmen:  revert_ap.sh --apply'
else
        say '== fertig: Ausgangszustand wiederhergestellt (hostapd/dnsmasq MASKED) =='
fi
