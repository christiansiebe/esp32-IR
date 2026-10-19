#!/bin/sh
# deploy_ap.sh - roll the Odroid WLAN access point out on the board.
#
# Consumes the files rendered by odroid_ap_config.py (hostapd.conf,
# odroid-ap.conf, odroid-ap-nat.sh) and installs them, in the order the plan
# ODROID-LAN-AP-KAMERA.md mandates (P1 end0-uplink must already hold):
#
#   1. install hostapd.conf + the dnsmasq drop-in + the NAT script
#   2. point /etc/default/hostapd at the config
#   3. systemctl unmask + enable --now hostapd dnsmasq
#   4. NetworkManager must let go of the radio (managed no)
#   5. put the gateway address on the AP interface and bring it up
#   6. apply the NAT/forwarding rules (idempotent -C/-A)
#   7. arm the dead-man switch (auto-revert after N minutes unless confirmed)
#
# Safety:
#   * dry-run is the DEFAULT. Nothing is written or switched on without --apply.
#     hostapd/dnsmasq stay masked on the board until someone decides to deploy.
#   * AP_ROOT is a path prefix for every board path, so the whole flow can run
#     against a scratch tree (tests) with no board and no root. When AP_ROOT is
#     set, system commands (systemctl/nmcli/iptables/ip) are recorded to
#     $AP_ROOT/var/log/odroid-ap-commands.log instead of being executed.
#   * Everything installed here is removed again by revert_ap.sh.
#
# Aufruf: deploy_ap.sh [--apply] [--root PREFIX] [--conf-dir DIR]
#                      [--ap-iface IF] [--up-iface IF] [--ap-addr CIDR]
#                      [--deadman MIN] [--no-deadman] [--confirm]
#   AP_ROOT      Pfad-Praefix fuer alle Board-Pfade (Tests/Dry-Run).
#   AP_CONF_DIR  Verzeichnis mit den generierten Dateien (sonst --conf-dir).
set -u

HERE=$(dirname "$(readlink -f "$0")")

APPLY=0
CONFIRM=0
DRYFORCE=0
DEADMAN=10          # minutes; 0 = do not arm
DEADMAN_SET=0
CONF_DIR=${AP_CONF_DIR:-}
AP_IFACE_OPT=
UP_IFACE_OPT=
AP_ADDR_OPT=

usage() {
        cat <<'EOF'
Aufruf: deploy_ap.sh [--apply] [--conf-dir DIR] [Optionen]

  (ohne --apply)  Dry-Run: zeigt nur, was passieren wuerde. Aendert NICHTS.
  --apply         wirklich ausrollen (root noetig, ausser AP_ROOT ist gesetzt).

  --conf-dir DIR  Verzeichnis mit hostapd.conf / odroid-ap.conf /
                  odroid-ap-nat.sh (aus odroid_ap_config.py --out DIR).
  --ap-iface IF   WLAN-Interface (Default: interface= aus hostapd.conf).
  --up-iface IF   Uplink-Interface (Default: end0).
  --ap-addr CIDR  Gateway-Adresse des AP-Subnetzes
                  (Default: router+dnsmasq-Netzmaske, z. B. 192.168.50.1/24).
  --deadman MIN   Auto-Revert nach MIN Minuten ohne Bestaetigung (Default 10).
  --no-deadman    Keinen Auto-Revert armieren.
  --confirm       Dead-Man bestaetigen: Auto-Revert abbrechen, AP bleibt aktiv.
                  Wirkt sofort (impliziert --apply); mit --dry-run nur Vorschau.
  -h, --help      Diese Hilfe.

Umgebung:
  AP_ROOT         Pfad-Praefix fuer alle Board-Pfade (fuer Tests/Dry-Run).
  AP_CONF_DIR     wie --conf-dir.

Beispiel:
  python3 odroid_ap_config.py --ssid "..." --passphrase "..." --out /tmp/ap
  sh deploy_ap.sh --conf-dir /tmp/ap            # Dry-Run
  sudo sh deploy_ap.sh --conf-dir /tmp/ap --apply
EOF
}

while [ $# -gt 0 ]; do
        case "$1" in
                --apply) APPLY=1 ;;
                --dry-run) APPLY=0; DRYFORCE=1 ;;
                --confirm) CONFIRM=1; APPLY=1 ;;
                --root) shift; AP_ROOT_OPT=${1:-} ;;
                --conf-dir) shift; CONF_DIR=${1:-} ;;
                --ap-iface) shift; AP_IFACE_OPT=${1:-} ;;
                --up-iface) shift; UP_IFACE_OPT=${1:-} ;;
                --ap-addr) shift; AP_ADDR_OPT=${1:-} ;;
                --deadman) shift; DEADMAN=${1:-}; DEADMAN_SET=1 ;;
                --no-deadman) DEADMAN=0; DEADMAN_SET=1 ;;
                -h|--help) usage; exit 0 ;;
                *) echo "unbekannte Option: $1" >&2; usage >&2; exit 2 ;;
        esac
        shift
done

ROOT=${AP_ROOT_OPT:-${AP_ROOT:-}}
[ "$DRYFORCE" = 1 ] && APPLY=0
DRY=$((1 - APPLY))

[ -n "$CONF_DIR" ] || CONF_DIR=$HERE/ap-config

# --- Board paths (all under ROOT) ------------------------------------------
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

die() { echo "FEHLER: $*" >&2; exit 1; }
say() { echo "$*"; }

# run: file operations. DRY prints, otherwise executes.
run() { if [ "$DRY" = 1 ]; then echo "   + $*"; else "$@"; fi; }

# sys: system commands (systemctl/nmcli/iptables/ip). DRY prints; with a ROOT
# prefix they are recorded to a log instead of executed (no board in tests).
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

# --- --confirm: cancel the dead-man switch, keep the AP --------------------
confirm_deadman() {
        say '== Dead-Man bestaetigen (AP bleibt aktiv) =='
        if [ "$DRY" = 1 ]; then
                say '   (dry-run: Marker/Timer bleiben unveraendert)'
        elif [ -f "$MARKER" ]; then
                rm -f "$MARKER"
                say '   Marker entfernt - kein Auto-Revert mehr.'
        else
                say '   Kein Marker vorhanden - nichts zu tun.'
        fi
        sys systemctl stop odroid-ap-deadman.timer
        sys systemctl disable odroid-ap-deadman.timer
        say ''
        if [ "$DRY" = 1 ]; then
                say 'Dry-Run: nichts geaendert. Wirklich bestaetigen: deploy_ap.sh --confirm --apply'
        else
                say 'AP-Konfiguration bleibt dauerhaft aktiv.'
        fi
        exit 0
}

# --- placeholder / sanity checks -------------------------------------------
validate_hostapd() {
        f=$1
        [ -s "$f" ] || die "$f fehlt oder ist leer"
        grep -qE '^[^#]*<(SSID|PASSWORT|PASSWORD|KANAL|CHANNEL)>' "$f" \
                && die "$f enthaelt noch Platzhalter (<SSID>/<PASSWORT>/...) - erst mit echten Werten erzeugen"
        grep -qiE '^[^#]*=(changeme|change-me|passwort|password|geheim)$' "$f" \
                && die "$f enthaelt einen Platzhalter-Wert - erst mit echten Werten erzeugen"
        ssid=$(sed -n 's/^ssid=//p' "$f" | head -1)
        psk=$(sed -n 's/^wpa_passphrase=//p' "$f" | head -1)
        ch=$(sed -n 's/^channel=//p' "$f" | head -1)
        [ -n "$ssid" ] || die "$f: ssid= fehlt/leer"
        [ -n "$psk" ] || die "$f: wpa_passphrase= fehlt/leer"
        [ "${#psk}" -ge 8 ] || die "$f: wpa_passphrase kuerzer als 8 Zeichen"
        [ -n "$ch" ] || die "$f: channel= fehlt/leer"
}

validate_dnsmasq() {
        f=$1
        [ -s "$f" ] || die "$f fehlt oder ist leer"
        grep -qE '^dhcp-range=' "$f" || die "$f: dhcp-range= fehlt"
        grep -qE '^dhcp-option=option:router,' "$f" || die "$f: dhcp-option router fehlt"
}

validate_nat() {
        f=$1
        [ -s "$f" ] || die "$f fehlt oder ist leer"
        grep -q 'MASQUERADE' "$f" || die "$f: keine MASQUERADE-Regel"
}

# --- AP interface / address from the generated files -----------------------
iface_from_hostapd() { sed -n 's/^interface=//p' "$CONF_DIR/hostapd.conf" 2>/dev/null | head -1; }

mask2prefix() {  # 255.255.255.0 -> 24
        echo "$1" | awk -F. '{s=0; for(i=1;i<=4;i++){v=$i+0; while(v>0){s+=v%2; v=int(v/2)}} print s}'
}

addr_from_dnsmasq() {
        gw=$(sed -n 's/^dhcp-option=option:router,//p' "$CONF_DIR/odroid-ap.conf" 2>/dev/null | head -1)
        mask=$(sed -n 's/^dhcp-range=[^,]*,[^,]*,//p' "$CONF_DIR/odroid-ap.conf" 2>/dev/null | head -1 | cut -d, -f1)
        [ -n "$gw" ] || return 0
        if [ -n "$mask" ]; then
                echo "$gw/$(mask2prefix "$mask")"
        else
                echo "$gw"
        fi
}

# AP subnet must not overlap the home network (defence in depth; the generator
# checks this too, but the files may have been hand-edited).
check_not_home_net() {
        case "$1" in
                192.168.178.*) die "AP-Adresse $1 liegt im Heimnetz 192.168.178.0/24" ;;
        esac
}

# --- main ------------------------------------------------------------------
[ "$DRY" = 1 ] || [ -n "$ROOT" ] || [ "$(id -u)" = 0 ] \
        || die "muss als root laufen (oder ohne --apply als Dry-Run)."

if [ "$CONFIRM" = 1 ]; then confirm_deadman; fi

say '== 1. Konfiguration pruefen =='
[ -d "$CONF_DIR" ] || die "Konfigurationsverzeichnis fehlt: $CONF_DIR (erst odroid_ap_config.py --out DIR laufen lassen)"
validate_hostapd "$CONF_DIR/hostapd.conf"
validate_dnsmasq "$CONF_DIR/odroid-ap.conf"
validate_nat "$CONF_DIR/odroid-ap-nat.sh"
say '   hostapd.conf / odroid-ap.conf / odroid-ap-nat.sh ok'

AP_IFACE=${AP_IFACE_OPT:-$(iface_from_hostapd)}
[ -n "$AP_IFACE" ] || AP_IFACE=wlxdc627949ba79
UP_IFACE=${UP_IFACE_OPT:-end0}
AP_ADDR=${AP_ADDR_OPT:-$(addr_from_dnsmasq)}
[ -n "$AP_ADDR" ] || AP_ADDR=192.168.50.1/24
check_not_home_net "$AP_ADDR"
say "   AP-Interface: $AP_IFACE   Uplink: $UP_IFACE   AP-Adresse: $AP_ADDR"

if [ "$DRY" = 1 ]; then
        say '   (dry-run: es wird nichts geaendert)'
else
        say "   (apply: Ziel-Praefix AP_ROOT=${ROOT:-/})"
fi

say '== 2. Dateien installieren =='
run mkdir -p "$(dirname "$HOSTAPD_CONF")" "$(dirname "$DNSMASQ_CONF")" \
        "$(dirname "$NAT_SH")" "$(dirname "$DEADMAN_SVC")"
run install -m 0644 "$CONF_DIR/hostapd.conf" "$HOSTAPD_CONF"
run install -m 0644 "$CONF_DIR/odroid-ap.conf" "$DNSMASQ_CONF"
run install -m 0755 "$CONF_DIR/odroid-ap-nat.sh" "$NAT_SH"
run install -m 0755 "$HERE/revert_ap.sh" "$REVERT_SH"

say '== 3. /etc/default/hostapd auf die Konfiguration zeigen lassen =='
DAEMON_LINE='DAEMON_CONF="/etc/hostapd/hostapd.conf"'
if [ "$DRY" = 1 ]; then
        echo "   + $HOSTAPD_DEF: $DAEMON_LINE (idempotent)"
elif [ -f "$HOSTAPD_DEF" ]; then
        [ -f "$HOSTAPD_DEF_BAK" ] || cp -a "$HOSTAPD_DEF" "$HOSTAPD_DEF_BAK"
        if grep -q '^DAEMON_CONF=' "$HOSTAPD_DEF"; then
                sed -i "s|^DAEMON_CONF=.*|$DAEMON_LINE|" "$HOSTAPD_DEF"
        else
                echo "$DAEMON_LINE" >> "$HOSTAPD_DEF"
        fi
else
        mkdir -p "$(dirname "$HOSTAPD_DEF")"
        echo "$DAEMON_LINE" > "$HOSTAPD_DEF"
fi

say '== 4. hostapd/dnsmasq entsperren und starten =='
sys systemctl unmask hostapd dnsmasq
sys systemctl enable --now hostapd dnsmasq

say '== 5. NetworkManager das Funkmodul freigeben =='
sys nmcli device set "$AP_IFACE" managed no

say '== 6. AP-Interface adressieren und hochfahren =='
sys ip addr replace "$AP_ADDR" dev "$AP_IFACE"
sys ip link set "$AP_IFACE" up

say '== 7. NAT/Forwarding setzen (idempotent) =='
sys sh "$NAT_SH"

say '== 8. Zustand merken (fuer revert_ap.sh) =='
if [ "$DRY" = 1 ]; then
        echo "   + $STATE schreiben"
else
        mkdir -p "$(dirname "$STATE")"
        {
                echo "AP_IFACE=$AP_IFACE"
                echo "UP_IFACE=$UP_IFACE"
                echo "AP_ADDR=$AP_ADDR"
                if [ -f "$HOSTAPD_DEF_BAK" ]; then
                        echo "HOSTAPD_DEF_HAS_BAK=1"
                fi
        } > "$STATE"
fi

if [ "$DEADMAN" != 0 ]; then
        say "== 9. Dead-Man-Switch armieren (Auto-Revert nach ${DEADMAN} min) =="
        run install -m 0755 "$HERE/odroid_ap_deadman.sh" "$DEADMAN_SH"
        if [ "$DRY" = 1 ]; then
                echo "   + $DEADMAN_SVC / $DEADMAN_TIMER schreiben (OnActiveSec=${DEADMAN}min)"
                echo "   + $MARKER setzen"
        else
                cat > "$DEADMAN_SVC" <<UNIT
[Unit]
Description=Auto-revert the Odroid AP config unless it was confirmed

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/odroid_ap_deadman.sh
UNIT
                cat > "$DEADMAN_TIMER" <<UNIT
[Unit]
Description=Arm the Odroid AP auto-revert after ${DEADMAN} minutes

[Timer]
OnActiveSec=${DEADMAN}min
Unit=odroid-ap-deadman.service

[Install]
WantedBy=timers.target
UNIT
                mkdir -p "$(dirname "$MARKER")"
                : > "$MARKER"
        fi
        sys systemctl daemon-reload
        sys systemctl enable --now odroid-ap-deadman.timer
else
        say '== 9. Dead-Man-Switch nicht angefordert (--no-deadman) =='
fi

say '== 10. Kontrolle =='
if [ "$DRY" = 1 ]; then
        echo "   + systemctl is-active hostapd dnsmasq / iw dev $AP_IFACE info"
else
        if [ -z "$ROOT" ]; then
                say "   hostapd: $(systemctl is-active hostapd 2>/dev/null || echo unbekannt)"
                say "   dnsmasq: $(systemctl is-active dnsmasq 2>/dev/null || echo unbekannt)"
                say "   iw:      $(iw dev "$AP_IFACE" info 2>/dev/null | sed -n 's/^[[:space:]]*//p' | tr '\n' ' ' || true)"
        fi
        say "   hostapd.conf: $( [ -f "$HOSTAPD_CONF" ] && echo vorhanden || echo FEHLT )"
        say "   Marker:       $( [ -f "$MARKER" ] && echo 'gesetzt (Auto-Revert scharf)' || echo 'keiner' )"
fi

say ''
if [ "$DRY" = 1 ]; then
        say '== dry-run fertig (nichts geaendert) =='
        say 'Wirklich ausrollen:  deploy_ap.sh --conf-dir DIR --apply'
else
        say '== fertig =='
        if [ "$DEADMAN" != 0 ]; then
                say "Auto-Revert ist scharf: ohne Bestaetigung baut das System nach ${DEADMAN} min zurueck."
                say 'AP geprueft und behalten?  ->  deploy_ap.sh --confirm'
        fi
        say 'Komplett zuruecknehmen:    revert_ap.sh --apply'
fi
