#!/bin/sh
# restore-stock-oc.sh - remove the OC test overlay and its boot guards.
#
# Undoes everything deploy_oc_safe.sh installed, using only the SD card:
#   * /boot/armbianEnv.txt: drop the oc entry from user_overlays (keeps any
#     other overlay, e.g. lowfreq), and drop cpufreq.default_governor=performance
#     ONLY if no other overlay is left.
#   * delete /boot/overlay-user/oc.dtbo and /etc/oc-test.pending.
#   * disable and delete oc-freq-guard.service and oc-test-hold.service
#     (plus their scripts/defaults).
#   * restore scaling_max_freq to cpuinfo_max_freq and scaling_min_freq to
#     1.0 GHz on every policy.
#
# Idempotent: running it twice is fine. Reboot afterwards.
#
# Aufruf: restore-stock-oc.sh [--dry-run]
#   OC_ROOT  Pfad-Praefix fuer alle Board-Pfade (fuer Dry-Run/Tests).
set -u

DRY=0
case "${1:-}" in
        --dry-run) DRY=1 ;;
        -h|--help)
                cat <<'EOF'
Aufruf: restore-stock-oc.sh [--dry-run]

Stellt die Werks-Takte her (OC-Testoverlay + Waechter entfernen):
  * /boot/armbianEnv.txt: oc aus user_overlays nehmen; Boot-Governor nur
    entfernen, wenn kein Overlay mehr aktiv ist
  * /boot/overlay-user/oc.dtbo und /etc/oc-test.pending loeschen
  * oc-freq-guard.service und oc-test-hold.service deaktivieren+loeschen
  * scaling_max_freq auf cpuinfo_max_freq, scaling_min_freq auf 1.0 GHz

  --dry-run      Nur anzeigen, was passieren wuerde (kein root noetig).
  -h, --help     Diese Hilfe.

Umgebung:
  OC_ROOT        Pfad-Praefix fuer alle Board-Pfade (fuer Dry-Run/Tests).

Idempotent, danach Reboot noetig.
EOF
                exit 0 ;;
        '') ;;
        *) echo "unbekannte Option: $1" >&2; exit 2 ;;
esac

ROOT=${OC_ROOT:-}

ENV=${ROOT}/boot/armbianEnv.txt
DTBO=${ROOT}/boot/overlay-user/oc.dtbo
MARKER=${ROOT}/etc/oc-test.pending
OCGUARD_SVC=${ROOT}/etc/systemd/system/oc-freq-guard.service
OCGUARD_SH=${ROOT}/usr/local/sbin/oc-freq-guard.sh
OCGUARD_DEF=${ROOT}/etc/default/oc-freq-guard
HOLD_SVC=${ROOT}/etc/systemd/system/oc-test-hold.service
HOLD_SH=${ROOT}/usr/local/sbin/oc_test_hold.sh
SYSFS=${ROOT}/sys/devices/system

die() { echo "FEHLER: $*" >&2; exit 1; }
say() { echo "$*"; }
run() { if [ "$DRY" = 1 ]; then echo "   + $*"; else "$@"; fi; }

# user_overlays um "oc" bereinigen, uebrige Namen behalten.
strip_oc_overlay() {
        if [ "$DRY" = 1 ]; then
                echo "   + sed -i 's/\\(^\\| \\)oc\\( \\|$\\)/ /g' $ENV  (user_overlays)"
                return 0
        fi
        [ -f "$ENV" ] || return 0
        sed -i '/^user_overlays=/ {
                s/user_overlays=//
                s/\(^\| \)oc\( \|$\)/ /g
                s/^ *//; s/ *$//
                s/^/user_overlays=/
        }' "$ENV"
}

[ -f "$ENV" ] || die "kein Board erkannt: $ENV fehlt (Odroid-Boot-Verzeichnis?). Nur dort ausfuehren."
if [ "$DRY" != 1 ] && [ -z "$ROOT" ]; then
        [ "$(id -u)" = 0 ] || die "muss als root laufen (oder --dry-run benutzen)."
fi

say '== Werkszustand (OC) wiederherstellen =='
[ "$DRY" = 1 ] && say '   (dry-run: es wird nichts geaendert)'

say ''
say '== 1. /boot/armbianEnv.txt bereinigen =='
strip_oc_overlay
# Boot-Governor nur entfernen, wenn kein Overlay mehr uebrig ist - ein anderes
# Overlay (z. B. lowfreq) braucht denselben Schutz.
if [ "$DRY" = 1 ]; then
        echo "   + Boot-Governor entfernen, falls user_overlays leer"
elif grep -q '^user_overlays=[A-Za-z0-9_ -]*[A-Za-z0-9_]' "$ENV"; then
        say '   anderes Overlay aktiv -> cpufreq.default_governor bleibt'
else
        sed -i 's/ cpufreq.default_governor=performance//' "$ENV"
fi
[ "$DRY" != 1 ] && { grep -nE 'user_overlays|extraargs' "$ENV" || true; }

say '== 2. Overlay und Marker entfernen =='
run rm -f "$DTBO"
run rm -f "$MARKER"

say '== 3. OC-Dienste deaktivieren und loeschen =='
for s in oc-freq-guard oc-test-hold; do
        run systemctl disable --now "$s.service"
done
run rm -f "$OCGUARD_SVC" "$OCGUARD_SH" "$OCGUARD_DEF"
run rm -f "$HOLD_SVC" "$HOLD_SH"
run systemctl daemon-reload
run systemctl reset-failed

say '== 4. Frequenzgrenzen auf Werkswerte =='
if [ "$DRY" = 1 ]; then
        echo "   + echo <cpuinfo_max_freq> > $SYSFS/cpu/cpufreq/policy*/scaling_max_freq"
        echo "   + echo 1000000 > $SYSFS/cpu/cpufreq/policy*/scaling_min_freq"
else
        for p in "$SYSFS"/cpu/cpufreq/policy*; do
                hard=$(cat "$p/cpuinfo_max_freq" 2>/dev/null)
                if [ -n "$hard" ] && [ -w "$p/scaling_max_freq" ]; then
                        echo "$hard" > "$p/scaling_max_freq" 2>/dev/null
                fi
                [ -w "$p/scaling_min_freq" ] && echo 1000000 > "$p/scaling_min_freq" 2>/dev/null
        done
        say "   online: $(cat "$SYSFS/cpu/online" 2>/dev/null || echo unbekannt)"
fi

say ''
say '== Zusammenfassung =='
say '  oc aus user_overlays entfernt, oc.dtbo + Marker geloescht'
say '  oc-freq-guard.service, oc-test-hold.service aus'
say '  scaling_max_freq auf Werksmaximum, scaling_min_freq auf 1.0 GHz'
say ''
say 'REBOOT NOETIG:'
say '  sync && reboot'
