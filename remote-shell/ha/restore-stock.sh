#!/bin/sh
# restore-stock.sh - bring the Odroid N2+ back to factory CPU settings.
#
# Undoes everything the lowfreq work installed, using only the SD card:
#   * /boot/armbianEnv.txt: drop user_overlays= and the
#     cpufreq.default_governor=performance boot arg.
#   * delete /boot/overlay-user/lowfreq.dtbo.
#   * disable and delete cpu-freq-guard.service and cpu-hotplug.service
#     (plus their scripts/defaults).
#   * /etc/default/cpufrequtils: MIN_SPEED=1000000, GOVERNOR=schedutil.
#   * bring every CPU back online.
#
# Idempotent: running it twice is fine. Reboot afterwards.
#
# Aufruf: restore-stock.sh [--dry-run]
#   --dry-run  Nur anzeigen, was passieren wuerde (kein root, kein Board noetig;
#              verlangt aber eine vorhandene /boot/armbianEnv.txt).
#   LOWFREQ_ROOT  Pfad-Praefix fuer alle Board-Pfade (fuer Dry-Run/Tests).
set -u

DRY=0
case "${1:-}" in
	--dry-run) DRY=1 ;;
	-h|--help)
		cat <<'EOF'
Aufruf: restore-stock.sh [--dry-run]

Stellt den Werkszustand des Odroid N2+ von der SD-Karte her:
  * /boot/armbianEnv.txt: user_overlays= und
    cpufreq.default_governor=performance entfernen
  * /boot/overlay-user/lowfreq.dtbo loeschen
  * cpu-freq-guard.service und cpu-hotplug.service deaktivieren+loeschen
  * /etc/default/cpufrequtils: MIN_SPEED=1000000, GOVERNOR=schedutil
  * alle CPUs wieder online

  --dry-run      Nur anzeigen, was passieren wuerde (kein root noetig).
  -h, --help     Diese Hilfe.

Umgebung:
  LOWFREQ_ROOT   Pfad-Praefix fuer alle Board-Pfade (fuer Dry-Run/Tests).

Idempotent, danach Reboot noetig.
EOF
		exit 0 ;;
	'') ;;
	*) echo "unbekannte Option: $1" >&2; exit 2 ;;
esac

ROOT=${LOWFREQ_ROOT:-}

ENV=${ROOT}/boot/armbianEnv.txt
DTBO=${ROOT}/boot/overlay-user/lowfreq.dtbo
GUARD_SVC=${ROOT}/etc/systemd/system/cpu-freq-guard.service
GUARD_SH=${ROOT}/usr/local/sbin/cpu-freq-guard.sh
GUARD_DEF=${ROOT}/etc/default/cpu-freq-guard
HOLD_SVC=${ROOT}/etc/systemd/system/lowfreq-test-hold.service
HOLD_SH=${ROOT}/usr/local/sbin/lowfreq_test_hold.sh
HOTPLUG_SVC=${ROOT}/etc/systemd/system/cpu-hotplug.service
HOTPLUG_PY=${ROOT}/usr/local/sbin/cpu_hotplug.py
CPUFREQUTILS=${ROOT}/etc/default/cpufrequtils
MARKER=${ROOT}/etc/lowfreq-test.pending
SYSFS=${ROOT}/sys/devices/system

die() { echo "FEHLER: $*" >&2; exit 1; }
say() { echo "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "   + $*"; else "$@"; fi; }

# Nur im Dry-Run: Datei-Inhalt mitteilen, statt zu aendern.
run_sed() {  # run_sed <datei> <sed-ausdruck>
	if [ "$DRY" = 1 ]; then
		echo "   + sed -i '$2' $1"
	elif [ -f "$1" ]; then
		sed -i "$2" "$1"
	fi
}

# --- Vorpruefung: ohne Board klar abbrechen --------------------------------
[ -f "$ENV" ] || die "kein Board erkannt: $ENV fehlt (Odroid-Boot-Verzeichnis?). Nur dort ausfuehren."
# root nur ohne Testwurzel verlangen (siehe deploy_lowfreq_safe.sh).
if [ "$DRY" != 1 ] && [ -z "$ROOT" ]; then
	[ "$(id -u)" = 0 ] || die "muss als root laufen (oder --dry-run benutzen)."
fi

say '== Werkszustand wiederherstellen =='
[ "$DRY" = 1 ] && say '   (dry-run: es wird nichts geaendert)'

say ''
say '== 1. /boot/armbianEnv.txt bereinigen =='
# Nur "lowfreq" entfernen; andere Overlay-Namen bleiben stehen.
run_sed "$ENV" '/^user_overlays=/ {
	s/user_overlays=//
	s/\(^\| \)lowfreq\( \|$\)/ /g
	s/^ *//; s/ *$//
	/^$/d
	s/^/user_overlays=/
}'
run_sed "$ENV" 's/ cpufreq.default_governor=performance//'
if [ "$DRY" != 1 ]; then
	grep -nE 'user_overlays|extraargs' "$ENV" || true
fi

say '== 2. Overlay entfernen =='
run rm -f "$DTBO"

say '== 3. Test-Marker entfernen =='
run rm -f "$MARKER"

say '== 4. Dienste deaktivieren und loeschen =='
for s in cpu-freq-guard cpu-hotplug lowfreq-test-hold; do
	run systemctl disable --now "$s.service"
done
run rm -f "$GUARD_SVC" "$GUARD_SH" "$GUARD_DEF"
run rm -f "$HOTPLUG_SVC" "$HOTPLUG_PY"
run rm -f "$HOLD_SVC" "$HOLD_SH"
run systemctl daemon-reload
run systemctl reset-failed

say '== 5. cpufrequtils auf Werkswerte =='
if [ "$DRY" = 1 ]; then
	echo "   + sed -i 's/^MIN_SPEED=.*/MIN_SPEED=1000000/' $CPUFREQUTILS"
	echo "   + sed -i 's/^GOVERNOR=.*/GOVERNOR=schedutil/' $CPUFREQUTILS"
else
	if [ -w "$CPUFREQUTILS" ]; then
		sed -i 's/^MIN_SPEED=.*/MIN_SPEED=1000000/' "$CPUFREQUTILS"
		grep -q '^MIN_SPEED=' "$CPUFREQUTILS" || echo 'MIN_SPEED=1000000' >> "$CPUFREQUTILS"
		sed -i 's/^GOVERNOR=.*/GOVERNOR=schedutil/' "$CPUFREQUTILS"
		grep -q '^GOVERNOR=' "$CPUFREQUTILS" || echo 'GOVERNOR=schedutil' >> "$CPUFREQUTILS"
		grep -nE '^(MIN_SPEED|GOVERNOR)=' "$CPUFREQUTILS"
	else
		say "   HINWEIS: $CPUFREQUTILS nicht beschreibbar - uebersprungen."
	fi
fi

say '== 6. Alle CPUs wieder online, Untergrenze sofort setzen =='
if [ "$DRY" = 1 ]; then
	echo "   + echo 1 > $SYSFS/cpu*/online"
	echo "   + echo 1000000 > $SYSFS/cpu/cpufreq/policy*/scaling_min_freq"
	echo "   + echo schedutil > $SYSFS/cpu/cpufreq/policy*/scaling_governor"
else
	for c in "$SYSFS"/cpu[0-9]*; do
		[ -w "$c/online" ] && echo 1 > "$c/online" 2>/dev/null
	done
	for p in "$SYSFS"/cpu/cpufreq/policy*; do
		[ -w "$p/scaling_min_freq" ] && echo 1000000 > "$p/scaling_min_freq" 2>/dev/null
		if grep -qw schedutil "$p/scaling_available_governors" 2>/dev/null &&
		   [ -w "$p/scaling_governor" ]; then
			echo schedutil > "$p/scaling_governor" 2>/dev/null
		fi
	done
	say "   online: $(cat "$SYSFS/cpu/online" 2>/dev/null || echo unbekannt)"
fi

say ''
say '== Zusammenfassung =='
say '  user_overlays entfernt, Boot-Governor-Argument entfernt'
say '  lowfreq.dtbo geloescht, Test-Marker geloescht'
say '  cpu-freq-guard.service, cpu-hotplug.service, lowfreq-test-hold.service aus'
say '  cpufrequtils: MIN_SPEED=1000000, GOVERNOR=schedutil'
say '  alle CPUs online, scaling_min_freq zurueck auf 1.0 GHz'
say ''
say 'REBOOT NOETIG, damit Werkswerte und Werks-DTB vollstaendig greifen:'
say '  sync && reboot'
