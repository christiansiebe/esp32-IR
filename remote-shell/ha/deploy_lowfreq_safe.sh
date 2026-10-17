#!/bin/sh
# deploy_lowfreq_safe.sh - install the lowfreq CPU overlay as a *test* overlay.
#
# Goal: try the extra low clock points on the Odroid N2+ without any risk of a
# boot loop. The overlay is active for exactly one boot and is then removed
# again automatically, unless it is explicitly promoted to a permanent install.
#
# How the one-boot window works (arm -> test -> auto-revert):
#   * This run leaves user_overlays= EMPTY in /boot/armbianEnv.txt and drops a
#     marker file /etc/lowfreq-test.pending. The running system therefore still
#     uses factory clocks when the script finishes.
#   * lowfreq-test-hold.service runs once per boot, right after sysinit:
#       marker present -> it writes user_overlays=lowfreq and DELETES the
#                         marker. The overlay is now armed for the next boot.
#       marker absent  -> it clears user_overlays back to empty, so the boot
#                         after the test returns to factory clocks.
#   * Boot sequence after a fresh deploy:
#       boot 1  factory clocks, hold service arms the overlay
#       boot 2  overlay active (the one test boot), hold service disarms it
#       boot 3  factory clocks again
#     i.e. reboot, reboot once more to test, reboot a third time to be back to
#     stock. `--promote` skips the auto-revert and keeps the overlay for good.
#
# Boot protection while the overlay is armed (same as deploy_lowfreq.sh):
#   * cpufreq.default_governor=performance keeps the early-boot governor at the
#     maximum frequency.
#   * cpu-freq-guard.service pins scaling_min_freq to 1.0 GHz right after
#     sysinit, so the new low points are only reachable by an explicit probe.
#
# Everything installed here is removed again by restore-stock.sh.
set -u

HERE=$(dirname "$(readlink -f "$0")")

DRY=0
ACTION=install          # install | promote | revert

usage() {
	cat <<'EOF'
Aufruf: deploy_lowfreq_safe.sh [--dry-run] [--promote] [--revert]

  (ohne Option)  Test-Overlay scharf machen. Der naechste Reboot liefert
                 Werkswerte, der Reboot danach einen einzigen Testboot mit
                 Overlay, danach wieder Werkswerte. Nichts wird dauerhaft
                 geaendert.
  --promote      Overlay dauerhaft uebernehmen: user_overlays=lowfreq bleibt
                 stehen und die Aufraeum-Unit wird entfernt.
  --revert       Test-Overlay zuruecknehmen (ruft restore-stock.sh auf).
  --dry-run      Nur anzeigen, was passieren wuerde. Braucht kein root und kein
                 Board, verlangt aber eine vorhandene /boot/armbianEnv.txt.
  -h, --help     Diese Hilfe.

Umgebung:
  LOWFREQ_ROOT   Pfad-Praefix fuer alle Board-Pfade (fuer Dry-Run/Tests).

Ohne root bricht das Skript ab (ausser mit --dry-run).
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

# Testwurzel: leer = echtes Board. Nur fuer --dry-run/Tests gedacht.
ROOT=${LOWFREQ_ROOT:-}

ENV=${ROOT}/boot/armbianEnv.txt
BOOTOVERLAY=${ROOT}/boot/overlay-user
DTB=${ROOT}/boot/dtb/amlogic/meson-g12b-odroid-n2-plus.dtb
DTBO=${BOOTOVERLAY}/lowfreq.dtbo
MARKER=${ROOT}/etc/lowfreq-test.pending
GUARD_SVC=${ROOT}/etc/systemd/system/cpu-freq-guard.service
GUARD_SH=${ROOT}/usr/local/sbin/cpu-freq-guard.sh
GUARD_DEF=${ROOT}/etc/default/cpu-freq-guard
HOLD_SVC=${ROOT}/etc/systemd/system/lowfreq-test-hold.service
HOLD_SH=${ROOT}/usr/local/sbin/lowfreq_test_hold.sh
CPUFREQUTILS=${ROOT}/etc/default/cpufrequtils

die() { echo "FEHLER: $*" >&2; exit 1; }
say() { echo "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "   + $*"; else "$@"; fi; }

# user_overlays auf einen Wert setzen (leer = kein Overlay).
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
# Ohne Board gibt es hier nichts zu tun: klar und frueh abbrechen.
[ -f "$ENV" ] || die "kein Board erkannt: $ENV fehlt (Odroid-Boot-Verzeichnis?). Nur dort ausfuehren."
# root nur ohne Testwurzel verlangen: mit LOWFREQ_ROOT wird in einen Testbaum
# geschrieben, das darf auch ein normaler Nutzer.
if [ "$DRY" != 1 ] && [ -z "$ROOT" ]; then
	[ "$(id -u)" = 0 ] || die "muss als root laufen (oder --dry-run benutzen)."
fi

# --- --revert: an restore-stock.sh abgeben ---------------------------------
if [ "$ACTION" = revert ]; then
	[ -x "$HERE/restore-stock.sh" ] || [ -f "$HERE/restore-stock.sh" ] || \
		die "restore-stock.sh liegt nicht neben $0"
	if [ "$DRY" = 1 ]; then
		exec sh "$HERE/restore-stock.sh" --dry-run
	else
		exec sh "$HERE/restore-stock.sh"
	fi
fi

# --- --promote: Overlay dauerhaft uebernehmen ------------------------------
if [ "$ACTION" = promote ]; then
	say '== Overlay dauerhaft uebernehmen (promote) =='
	set_user_overlays lowfreq
	run rm -f "$MARKER"
	say '   + Aufraeum-Unit entfernen (sonst loescht sie die Zeile beim Boot)'
	run systemctl disable --now lowfreq-test-hold.service
	run rm -f "$HOLD_SVC" "$HOLD_SH"
	run systemctl daemon-reload
	say ''
	say 'Overlay bleibt jetzt dauerhaft aktiv (user_overlays=lowfreq).'
	say 'Die Untergrenze haelt cpu-freq-guard.service; die tiefen Takte gibt es'
	say 'nur ueber lowfreq_set_floor.sh. Reboot noetig.'
	exit 0
fi

# --- Normalfall: Test-Overlay scharf machen --------------------------------
say "== 1. Overlay bauen und pruefen (Quelle: $HERE) =="
DTS=$HERE/lowfreq.dts
[ -f "$DTS" ] || DTS=/root/lowfreq.dts
if [ -f "$DTS" ] && command -v dtc >/dev/null 2>&1; then
	run dtc -@ -I dts -O dtb -o /tmp/lowfreq.dtbo "$DTS" || die "dtc fehlgeschlagen"
	[ -f "$DTB" ] || die "$DTB fehlt - Overlay nicht gegen das Board-DTB pruefbar"
	run fdtoverlay -i "$DTB" -o /tmp/merged.dtb /tmp/lowfreq.dtbo || die "fdtoverlay fehlgeschlagen"
	if [ "$DRY" != 1 ]; then
		dtc -I dtb -O dts /tmp/merged.dtb > /tmp/merged.dts 2>/dev/null
		for f in 666666666 500000000 250000000 200000000 125000000 100000000 62500000 50000000 40000000 31250000 25000000 20000000 15625000 24000000; do
			# Anker " {" noetig: "opp-100000000" ist Praefix von "opp-1000000000".
			n=$(grep -c "opp-$f {" /tmp/merged.dts)
			[ "$n" = 2 ] || die "opp-$f $n mal im gemergten DTB (erwartet 2)"
		done
		say '   alle neuen OPPs vorhanden (je 2)'
	fi
else
	say '   HINWEIS: dtc oder lowfreq.dts fehlt - ueberspringe die Bau-/Pruefstufe.'
	say '   Das Test-Overlay wird erst beim Reboot geladen und dann vom Kernel'
	say '   validiert; ein kaputtes Overlay wird von u-boot verworfen.'
fi

say '== 2. Overlay ablegen (noch NICHT in user_overlays) =='
run mkdir -p "$BOOTOVERLAY"
SRC_DTBO=$HERE/lowfreq.dtbo
[ -f "$SRC_DTBO" ] || SRC_DTBO=/tmp/lowfreq.dtbo
[ -f "$SRC_DTBO" ] || SRC_DTBO=/root/lowfreq.dtbo
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

say '== 4. Boot-Schutz: cpu-freq-guard.service (Floor 1.0 GHz) =='
GSRC=$HERE/cpu-freq-guard.sh
[ -f "$GSRC" ] || GSRC=/root/cpu-freq-guard.sh
SSRC=$HERE/cpu-freq-guard.service
[ -f "$SSRC" ] || SSRC=/root/cpu-freq-guard.service
[ -f "$GSRC" ] && [ -f "$SSRC" ] || die "cpu-freq-guard.sh/.service nicht gefunden"
run install -m 0755 "$GSRC" "$GUARD_SH"
if [ "$DRY" = 1 ]; then
	echo "   + echo 1000000 > $GUARD_DEF"
else
	echo 1000000 > "$GUARD_DEF"
fi
run install -m 0644 "$SSRC" "$GUARD_SVC"
run systemctl daemon-reload
run systemctl enable cpu-freq-guard.service
run systemctl start cpu-freq-guard.service

say '== 5. Aufraeum-Unit installieren (einmal pro Boot) =='
if [ "$DRY" = 1 ]; then
	echo "   + install $HOLD_SH"
	echo "   + install $HOLD_SVC"
else
	cat > "$HOLD_SH" <<'HOLD'
#!/bin/sh
# lowfreq_test_hold.sh - runs once per boot.
#
# Marker present : arm the lowfreq overlay for the next boot, delete the marker.
# Marker absent  : clear user_overlays so the boot after the test is stock again.
set -u
# LOWFREQ_ROOT ist beim echten Boot leer; der Praefix dient nur den Tests.
ENV=${LOWFREQ_ROOT:-}/boot/armbianEnv.txt
MARKER=${LOWFREQ_ROOT:-}/etc/lowfreq-test.pending
[ -f "$ENV" ] || exit 0
if [ -f "$MARKER" ]; then
	if grep -q '^user_overlays=' "$ENV"; then
		sed -i 's/^user_overlays=.*/user_overlays=lowfreq/' "$ENV"
	else
		echo 'user_overlays=lowfreq' >> "$ENV"
	fi
	rm -f "$MARKER"
	logger -t lowfreq-test-hold 'lowfreq overlay armed for the next boot'
else
	sed -i 's/^user_overlays=.*/user_overlays=/' "$ENV"
	logger -t lowfreq-test-hold 'no marker - boot stays on factory clocks'
fi
exit 0
HOLD
	chmod 0755 "$HOLD_SH"
	cat > "$HOLD_SVC" <<'UNIT'
[Unit]
Description=Arm the lowfreq test overlay for one boot, then drop it again
# Braucht armbianEnv.txt und das gefuellte sysfs.
After=sysinit.target cpu-freq-guard.service
Wants=cpu-freq-guard.service
Before=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/lowfreq_test_hold.sh

[Install]
WantedBy=multi-user.target
UNIT
fi
run systemctl daemon-reload
run systemctl enable lowfreq-test-hold.service

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
	say "   hold:   $(systemctl is-enabled lowfreq-test-hold.service 2>/dev/null || echo unbekannt)"
fi

say ''
say '== fertig =='
say 'Test-Overlay ist scharf, aber noch NICHT aktiv. Erwarteter Ablauf:'
say '  Reboot 1 -> Werkswerte (Unit traegt das Overlay ein)'
say '  Reboot 2 -> ein Testboot mit den tiefen Takten (Unit traegt es wieder aus)'
say '  Reboot 3 -> Werkswerte'
say 'Dauerhaft uebernehmen: deploy_lowfreq_safe.sh --promote'
say 'Komplett zuruecknehmen: restore-stock.sh'
