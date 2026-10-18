#!/bin/sh
# test_oc_safe.sh - host-side checks for deploy_oc_safe.sh and restore-stock-oc.sh.
# No board, no root: everything runs against a temporary OC_ROOT with a stub
# systemctl.
#
# Aufruf: sh remote-shell/ha/test_oc_safe.sh
set -u

HERE=$(dirname "$(readlink -f "$0")")
FAIL=0
ok()  { echo "ok   - $*"; }
bad() { echo "FAIL - $*"; FAIL=$((FAIL + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (erwartet '$3', war '$2')"; fi; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAKE="$WORK/fakebin"
ROOT="$WORK/root"
mkdir -p "$FAKE"

cat > "$FAKE/systemctl" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$FAKE/systemctl"
export PATH="$FAKE:$PATH"

new_root() {
        rm -rf "$ROOT"
        mkdir -p "$ROOT/boot/overlay-user" "$ROOT/etc/systemd/system" \
                "$ROOT/etc/default" "$ROOT/usr/local/sbin" \
                "$ROOT/sys/devices/system/cpu/cpufreq/policy0" \
                "$ROOT/sys/devices/system/cpu/cpufreq/policy2"
        printf 'verbosity=1\nuser_overlays=\nextraargs=rootdev=UUID=1234\n' > "$ROOT/boot/armbianEnv.txt"
        printf 'MIN_SPEED=15625\nGOVERNOR=schedutil\n' > "$ROOT/etc/default/cpufrequtils"
        : > "$ROOT/boot/overlay-user/lowfreq.dtbo"
}

env_line() { grep "^$1=" "$ROOT/boot/armbianEnv.txt" 2>/dev/null || true; }

# --- 1. ohne Board: klarer Abbruch -----------------------------------------
out=$(OC_ROOT="$WORK/leer" sh "$HERE/deploy_oc_safe.sh" --dry-run 2>&1)
case "$out" in *"kein Board erkannt"*) ok "deploy bricht ohne Board ab" ;; *) bad "deploy Abbruch" ;; esac
out=$(OC_ROOT="$WORK/leer" sh "$HERE/restore-stock-oc.sh" --dry-run 2>&1)
case "$out" in *"kein Board erkannt"*) ok "restore bricht ohne Board ab" ;; *) bad "restore Abbruch" ;; esac

# --- 2. --help -------------------------------------------------------------
sh "$HERE/deploy_oc_safe.sh" --help | grep -q 'Aufruf:' && ok "deploy --help" || bad "deploy --help"
sh "$HERE/restore-stock-oc.sh" --help | grep -q 'Aufruf:' && ok "restore --help" || bad "restore --help"

# --- 3. deploy laesst user_overlays leer, setzt Marker ---------------------
new_root
OC_ROOT="$ROOT" sh "$HERE/deploy_oc_safe.sh" >/dev/null 2>&1
check "deploy: user_overlays leer" "$(env_line user_overlays)" "user_overlays="
[ -f "$ROOT/etc/oc-test.pending" ] && ok "deploy: Marker gesetzt" || bad "deploy: Marker fehlt"
case "$(env_line extraargs)" in
        *performance*) ok "deploy: Boot-Governor gesetzt" ;;
        *) bad "deploy: Boot-Governor fehlt" ;;
esac
[ -f "$ROOT/etc/systemd/system/oc-freq-guard.service" ] && ok "deploy: oc-guard-Unit" || bad "deploy: oc-guard-Unit fehlt"
[ -f "$ROOT/usr/local/sbin/oc-freq-guard.sh" ] && ok "deploy: oc-guard-Skript" || bad "deploy: oc-guard-Skript fehlt"
[ -f "$ROOT/etc/default/oc-freq-guard" ] && ok "deploy: oc-guard-Cap" || bad "deploy: oc-guard-Cap fehlt"
check "deploy: Cap = Werkswert" "$(tr -dc 0-9 < "$ROOT/etc/default/oc-freq-guard")" "2400000"
[ -f "$ROOT/etc/systemd/system/oc-test-hold.service" ] && ok "deploy: hold-Unit" || bad "deploy: hold-Unit fehlt"

# --- 4. Boot-Sequenz (ohne anderes Overlay) --------------------------------
HOLD="$ROOT/usr/local/sbin/oc_test_hold.sh"
OC_ROOT="$ROOT" sh "$HOLD" >/dev/null 2>&1
check "boot 1: Overlay scharf" "$(env_line user_overlays)" "user_overlays=oc"
[ -f "$ROOT/etc/oc-test.pending" ] && bad "boot 1: Marker blieb liegen" || ok "boot 1: Marker verbraucht"
OC_ROOT="$ROOT" sh "$HOLD" >/dev/null 2>&1
check "boot 2: zurueck auf Werkswerte" "$(env_line user_overlays)" "user_overlays="
OC_ROOT="$ROOT" sh "$HOLD" >/dev/null 2>&1
check "boot 3: idempotent" "$(env_line user_overlays)" "user_overlays="

# --- 4b. Boot-Sequenz mit zweitem Overlay (lowfreq bleibt) -----------------
new_root
printf 'verbosity=1\nuser_overlays=lowfreq\nextraargs=rootdev=UUID=1234\n' > "$ROOT/boot/armbianEnv.txt"
OC_ROOT="$ROOT" sh "$HERE/deploy_oc_safe.sh" >/dev/null 2>&1
check "deploy: lowfreq bleibt" "$(env_line user_overlays)" "user_overlays=lowfreq"
OC_ROOT="$ROOT" sh "$ROOT/usr/local/sbin/oc_test_hold.sh" >/dev/null 2>&1
check "boot 1: oc dazu" "$(env_line user_overlays)" "user_overlays=lowfreq oc"
OC_ROOT="$ROOT" sh "$ROOT/usr/local/sbin/oc_test_hold.sh" >/dev/null 2>&1
check "boot 2: nur oc faellt" "$(env_line user_overlays)" "user_overlays=lowfreq"

# --- 5. promote ------------------------------------------------------------
new_root
OC_ROOT="$ROOT" sh "$HERE/deploy_oc_safe.sh" >/dev/null 2>&1
OC_ROOT="$ROOT" sh "$HERE/deploy_oc_safe.sh" --promote >/dev/null 2>&1
check "promote: Overlay bleibt" "$(env_line user_overlays)" "user_overlays=oc"
[ -f "$ROOT/etc/oc-test.pending" ] && bad "promote: Marker blieb" || ok "promote: Marker weg"
[ -f "$ROOT/etc/systemd/system/oc-test-hold.service" ] && bad "promote: hold-Unit blieb" || ok "promote: hold-Unit weg"

# --- 6. restore-stock-oc: anderer Overlay (lowfreq) bleibt ----------------
new_root
# Beide Overlays gleichzeitig aktiv (z. B. oc promoted, lowfreq dazu).
printf 'verbosity=1\nuser_overlays=lowfreq oc\nextraargs=rootdev=UUID=1234 cpufreq.default_governor=performance\n' > "$ROOT/boot/armbianEnv.txt"
: > "$ROOT/boot/overlay-user/oc.dtbo"
OC_ROOT="$ROOT" sh "$HERE/restore-stock-oc.sh" >/dev/null 2>&1
check "restore: oc aus user_overlays weg" "$(env_line user_overlays)" "user_overlays=lowfreq"
case "$(env_line extraargs)" in
        *performance*) ok "restore: Boot-Governor bleibt (lowfreq aktiv)" ;;
        *) bad "restore: Boot-Governor zu frueh entfernt" ;;
esac
[ -f "$ROOT/boot/overlay-user/oc.dtbo" ] && bad "restore: oc.dtbo blieb" || ok "restore: oc.dtbo weg"
[ -f "$ROOT/boot/overlay-user/lowfreq.dtbo" ] && ok "restore: lowfreq.dtbo bleibt" || bad "restore: lowfreq.dtbo weg"
[ -f "$ROOT/etc/oc-test.pending" ] && bad "restore: Marker blieb" || ok "restore: Marker weg"
for f in etc/systemd/system/oc-freq-guard.service etc/systemd/system/oc-test-hold.service \
         usr/local/sbin/oc-freq-guard.sh usr/local/sbin/oc_test_hold.sh etc/default/oc-freq-guard; do
        [ -e "$ROOT/$f" ] && bad "restore: $f blieb" || ok "restore: $f weg"
done

# --- 7. restore: letzter Overlay -> Boot-Governor faellt mit --------------
new_root
# nur oc aktiv machen, kein lowfreq
printf 'verbosity=1\nuser_overlays=oc\nextraargs=rootdev=UUID=1234 cpufreq.default_governor=performance\n' > "$ROOT/boot/armbianEnv.txt"
OC_ROOT="$ROOT" sh "$HERE/restore-stock-oc.sh" >/dev/null 2>&1
check "restore: user_overlays leer" "$(env_line user_overlays)" "user_overlays="
case "$(env_line extraargs)" in
        *performance*) bad "restore: Boot-Governor blieb ohne Overlay" ;;
        *) ok "restore: Boot-Governor weg ohne Overlay" ;;
esac

# --- 8. restore idempotent -------------------------------------------------
OC_ROOT="$ROOT" sh "$HERE/restore-stock-oc.sh" >/dev/null 2>&1 &&
        ok "restore: zweiter Lauf ok" || bad "restore: zweiter Lauf fehlgeschlagen"

# --- 9. oc-freq-guard.sh deckelt auf Werkswert ----------------------------
new_root
printf '2800000\n' > "$ROOT/sys/devices/system/cpu/cpufreq/policy2/scaling_max_freq"
printf '2400000\n' > "$ROOT/sys/devices/system/cpu/cpufreq/policy2/cpuinfo_max_freq"
printf '1000000\n' > "$ROOT/sys/devices/system/cpu/cpufreq/policy2/scaling_min_freq"
printf '2016000\n' > "$ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq"
printf '2016000\n' > "$ROOT/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq"
printf '1000000\n' > "$ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"
# Skript liest feste Pfade; ueber OC_ROOT laeuft es nicht. Deshalb hier die
# reine Logik mit OC_FREQ_GUARD_SYS als Praefix pruefen (s. oc-freq-guard.sh).
OC_FREQ_GUARD_SYS="$ROOT/sys/devices/system/cpu/cpufreq" \
        sh "$HERE/oc-freq-guard.sh" >/dev/null 2>&1
check "oc-guard: big auf Werkswert" "$(cat "$ROOT/sys/devices/system/cpu/cpufreq/policy2/scaling_max_freq")" "2400000"
check "oc-guard: little unveraendert" "$(cat "$ROOT/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq")" "2016000"

echo
if [ "$FAIL" = 0 ]; then echo "ALLE TESTS OK"; exit 0; else echo "$FAIL Test(s) fehlgeschlagen"; exit 1; fi
