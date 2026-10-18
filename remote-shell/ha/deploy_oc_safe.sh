#!/bin/sh
# deploy_oc_safe.sh - install the OC CPU overlay (2496 MHz, big cluster) as a
# *test* overlay: active for exactly one boot, then removed again, unless it is
# explicitly promoted to a permanent install.
#
# Same one-boot window as deploy_lowfreq_safe.sh (arm -> test -> auto-revert):
#   * This run leaves user_overlays= EMPTY in /boot/armbianEnv.txt and drops a
#     marker /etc/oc-test.pending. The running system keeps factory clocks.
#   * oc-test-hold.service runs once per boot, right after sysinit:
#       marker present -> writes user_overlays=oc and DELETES the marker.
#       marker absent  -> clears user_overlays back to empty.
#   * Boot sequence after a fresh deploy:
#       boot 1  factory clocks, hold service arms the overlay
#       boot 2  overlay active (the one test boot), hold service disarms it
#       boot 3  factory clocks again
#     `--promote` skips the auto-revert and keeps the overlay for good.
#
# Boot protection while the overlay is armed:
#   * cpufreq.default_governor=performance keeps the early-boot governor at the
#     maximum frequency (no silent jump to the undervolted OC point).
#   * cpu-freq-guard.service pins scaling_min_freq to 1.0 GHz (the low points,
#     if the overlay carries them, are not auto-selected).
#   * oc-freq-guard.service caps scaling_max_freq back to the stock 2400 MHz,
#     so the 2496 MHz point is only reachable by an explicit runtime raise.
#
# Everything installed here is removed again by restore-stock-oc.sh.
set -u

HERE=$(dirname "$(readlink -f "$0")")

DRY=0
ACTION=install          # install | promote | revert

usage() {
        cat <<'EOF'
Aufruf: deploy_oc_safe.sh [--dry-run] [--promote] [--revert]

  (ohne Option)  Test-Overlay scharf machen. Der naechste Reboot liefert
                 Werkswerte, der Reboot danach einen einzigen Testboot mit
                 Overlay, danach wieder Werkswerte.
  --promote      Overlay dauerhaft uebernehmen (user_overlays=oc bleibt).
  --revert       Test-Overlay zuruecknehmen (ruft restore-stock-oc.sh auf).
  --dry-run      Nur anzeigen, was passieren wuerde (kein root, kein Board).
  -h, --help     Diese Hilfe.

Umgebung:
  OC_ROOT        Pfad-Praefix fuer alle Board-Pfade (fuer Dry-Run/Tests).
  OC_DTS         Quell-DTS (Standard: oc.dts neben diesem Skript).
EOF
}

while [ $# -gt 0 ]; do
        case "$1" in
                --dry-run) DRY=1 ;;
                --promote) ACTION=promote ;;
                --revert)  ACTION=revert ;;
                -h|--help) usage; exit 0 ;;
                *) echo "unbekannte Option: $1" >&2; usage >&2; exit 2 ;;
        esac
        shift
done

ROOT=${OC_ROOT:-}

ENV=${ROOT}/boot/armbianEnv.txt
BOOTOVERLAY=${ROOT}/boot/overlay-user
DTB=${ROOT}/boot/dtb/amlogic/meson-g12b-odroid-n2-plus.dtb
DTBO=${BOOTOVERLAY}/oc.dtbo
MARKER=${ROOT}/etc/oc-test.pending
GUARD_SVC=${ROOT}/etc/systemd/system/cpu-freq-guard.service
GUARD_SH=${ROOT}/usr/local/sbin/cpu-freq-guard.sh
GUARD_DEF=${ROOT}/etc/default/cpu-freq-guard
OCGUARD_SVC=${ROOT}/etc/systemd/system/oc-freq-guard.service
OCGUARD_SH=${ROOT}/usr/local/sbin/oc-freq-guard.sh
OCGUARD_DEF=${ROOT}/etc/default/oc-freq-guard
HOLD_SVC=${ROOT}/etc/systemd/system/oc-test-hold.service
HOLD_SH=${ROOT}/usr/local/sbin/oc_test_hold.sh

die() { echo "FEHLER: $*" >&2; exit 1; }
say() { echo "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "   + $*"; else "$@"; fi; }

set_user_overlays() {  # set_user_overlays <wert>
        if [ "$DRY" = 1 ]; then
                echo "   + armbianEnv.txt: user_overlays=$1"
                return 0
        fi
        if grep -q '^user_overlays=' "$ENV"; then
                sed -i "s/^user_overlays=.*/user_overlays=$1/" "$ENV"
        else
                echo "user_overlays=$1" >> "$ENV"
        fi
}

# --- Vorpruefungen ---------------------------------------------------------
[ -f "$ENV" ] || die "kein Board erkannt: $ENV fehlt (Odroid-Boot-Verzeichnis?). Nur dort ausfuehren."
if [ "$DRY" != 1 ] && [ -z "$ROOT" ]; then
        [ "$(id -u)" = 0 ] || die "muss als root laufen (oder --dry-run benutzen)."
fi

# --- --revert: an restore-stock-oc.sh abgeben ------------------------------
if [ "$ACTION" = revert ]; then
        [ -f "$HERE/restore-stock-oc.sh" ] || die "restore-stock-oc.sh liegt nicht neben $0"
        if [ "$DRY" = 1 ]; then
                exec sh "$HERE/restore-stock-oc.sh" --dry-run
        else
                exec sh "$HERE/restore-stock-oc.sh"
        fi
fi

# --- --promote: Overlay dauerhaft uebernehmen ------------------------------
if [ "$ACTION" = promote ]; then
        say '== Overlay dauerhaft uebernehmen (promote) =='
        set_user_overlays oc
        run rm -f "$MARKER"
        say '   + Aufraeum-Unit entfernen (sonst loescht sie die Zeile beim Boot)'
        run systemctl disable --now oc-test-hold.service
        run rm -f "$HOLD_SVC" "$HOLD_SH"
        run systemctl daemon-reload
        say ''
        say 'Overlay bleibt dauerhaft aktiv (user_overlays=oc).'
        say 'Der OC-Punkt bleibt durch oc-freq-guard.service gedeckelt; er ist nur'
        say 'ueber ein ausdrueckliches Anheben von scaling_max_freq erreichbar.'
        exit 0
fi

# --- Normalfall: Test-Overlay scharf machen --------------------------------
say '== 1. Overlay bauen und pruefen =='
DTS=${OC_DTS:-$HERE/oc.dts}
[ -f "$DTS" ] || DTS=/root/oc.dts
if [ -f "$DTS" ] && command -v dtc >/dev/null 2>&1; then
        run dtc -@ -I dts -O dtb -o /tmp/oc.dtbo "$DTS" || die "dtc fehlgeschlagen"
        [ -f "$DTB" ] || die "$DTB fehlt - Overlay nicht gegen das Board-DTB pruefbar"
        run fdtoverlay -i "$DTB" -o /tmp/merged-oc.dtb /tmp/oc.dtbo || die "fdtoverlay fehlgeschlagen"
        if [ "$DRY" != 1 ]; then
                dtc -I dtb -O dts /tmp/merged-oc.dtb > /tmp/merged-oc.dts 2>/dev/null
                # Anker " {" noetig: "opp-2400000" ist Praefix von "opp-24000000".
                n=$(grep -c "opp-2496000000 {" /tmp/merged-oc.dts)
                [ "$n" = 1 ] || die "opp-2496000000 $n mal im gemergten DTB (erwartet 1)"
                say '   OC-OPP 2496 MHz im gemergten DTB vorhanden'
        fi
else
        say '   HINWEIS: dtc oder oc.dts fehlt - ueberspringe die Bau-/Pruefstufe.'
        say '   Das Overlay wird erst beim Reboot geladen und dann vom Kernel'
        say '   validiert; ein kaputtes Overlay wird von u-boot verworfen.'
fi

say '== 2. Overlay ablegen (noch NICHT in user_overlays) =='
run mkdir -p "$BOOTOVERLAY"
SRC_DTBO=$HERE/oc.dtbo
[ -f "$SRC_DTBO" ] || SRC_DTBO=/tmp/oc.dtbo
[ -f "$SRC_DTBO" ] || SRC_DTBO=/root/oc.dtbo
if [ -f "$SRC_DTBO" ]; then
        run cp "$SRC_DTBO" "$DTBO"
else
        say "   HINWEIS: keine .dtbo gefunden ($SRC_DTBO) - beim Reboot wird nichts geladen."
fi

say '== 3. armbianEnv: user_overlays leer lassen, Boot-Governor sichern =='
set_user_overlays ''
if [ "$DRY" = 1 ]; then
        echo "   + armbianEnv.txt: extraargs += cpufreq.default_governor=performance"
elif ! grep -q 'cpufreq.default_governor=performance' "$ENV"; then
        sed -i 's/^extraargs=\(.*\)$/extraargs=\1 cpufreq.default_governor=performance/' "$ENV"
fi

say '== 4. Boot-Schutz: Untergrenze (cpu-freq-guard) + Obergrenze (oc-freq-guard) =='
GSRC=$HERE/cpu-freq-guard.sh
[ -f "$GSRC" ] || GSRC=/root/cpu-freq-guard.sh
SSRC=$HERE/cpu-freq-guard.service
[ -f "$SSRC" ] || SSRC=/root/cpu-freq-guard.service
OSRC=$HERE/oc-freq-guard.sh
[ -f "$OSRC" ] || OSRC=/root/oc-freq-guard.sh
[ -f "$GSRC" ] && [ -f "$SSRC" ] || die "cpu-freq-guard.sh/.service nicht gefunden"
[ -f "$OSRC" ] || die "oc-freq-guard.sh nicht gefunden"
run install -m 0755 "$GSRC" "$GUARD_SH"
run install -m 0755 "$OSRC" "$OCGUARD_SH"
if [ "$DRY" = 1 ]; then
        echo "   + echo 1000000 > $GUARD_DEF"
        echo "   + echo 2400000 > $OCGUARD_DEF"
else
        echo 1000000 > "$GUARD_DEF"
        echo 2400000 > "$OCGUARD_DEF"
fi
run install -m 0644 "$SSRC" "$GUARD_SVC"
run install -m 0644 "$HERE/oc-freq-guard.service" "$OCGUARD_SVC"
run systemctl daemon-reload
run systemctl enable cpu-freq-guard.service oc-freq-guard.service
run systemctl start cpu-freq-guard.service oc-freq-guard.service

say '== 5. Aufraeum-Unit installieren (einmal pro Boot) =='
if [ "$DRY" = 1 ]; then
        echo "   + install $HOLD_SH"
        echo "   + install $HOLD_SVC"
else
        cat > "$HOLD_SH" <<'HOLD'
#!/bin/sh
# oc_test_hold.sh - runs once per boot.
#
# Marker present : arm the oc overlay for the next boot, delete the marker.
# Marker absent  : clear user_overlays so the boot after the test is stock again.
set -u
# OC_ROOT ist beim echten Boot leer; der Praefix dient nur den Tests.
ENV=${OC_ROOT:-}/boot/armbianEnv.txt
MARKER=${OC_ROOT:-}/etc/oc-test.pending
[ -f "$ENV" ] || exit 0
if [ -f "$MARKER" ]; then
        if grep -q '^user_overlays=' "$ENV"; then
                sed -i 's/^user_overlays=.*/user_overlays=oc/' "$ENV"
        else
                echo 'user_overlays=oc' >> "$ENV"
        fi
        rm -f "$MARKER"
        logger -t oc-test-hold 'oc overlay armed for the next boot'
else
        sed -i 's/^user_overlays=.*/user_overlays=/' "$ENV"
        logger -t oc-test-hold 'no marker - boot stays on factory clocks'
fi
exit 0
HOLD
        chmod 0755 "$HOLD_SH"
        cat > "$HOLD_SVC" <<'UNIT'
[Unit]
Description=Arm the OC test overlay for one boot, then drop it again
After=sysinit.target cpu-freq-guard.service oc-freq-guard.service
Wants=cpu-freq-guard.service oc-freq-guard.service
Before=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/oc_test_hold.sh

[Install]
WantedBy=multi-user.target
UNIT
fi
run systemctl daemon-reload
run systemctl enable oc-test-hold.service

say '== 6. Marker setzen (ein Testboot) =='
if [ "$DRY" = 1 ]; then
        echo "   + touch $MARKER"
else
        : > "$MARKER"
        chmod 0644 "$MARKER"
fi

say '== 7. Kontrolle =='
if [ "$DRY" = 1 ]; then
        echo "   + grep -nE 'user_overlays|extraargs' $ENV"
else
        grep -nE 'user_overlays|extraargs' "$ENV"
        say "   Marker: $( [ -f "$MARKER" ] && echo vorhanden || echo FEHLT )"
        say "   guard:  $(systemctl is-active cpu-freq-guard.service 2>/dev/null || echo unbekannt)"
        say "   ocguard:$(systemctl is-active oc-freq-guard.service 2>/dev/null || echo unbekannt)"
        say "   hold:   $(systemctl is-enabled oc-test-hold.service 2>/dev/null || echo unbekannt)"
fi

say ''
say '== fertig =='
say 'Test-Overlay ist scharf, aber noch NICHT aktiv. Erwarteter Ablauf:'
say '  Reboot 1 -> Werkswerte (Unit traegt das Overlay ein)'
say '  Reboot 2 -> ein Testboot mit dem OC-Punkt (Unit traegt es wieder aus)'
say '  Reboot 3 -> Werkswerte'
say 'Dauerhaft uebernehmen: deploy_oc_safe.sh --promote'
say 'Komplett zuruecknehmen: restore-stock-oc.sh'
