#!/bin/sh
# test_lowfreq_safe.sh - host-side checks for deploy_lowfreq_safe.sh and
# restore-stock.sh. No board, no root: everything runs against a temporary
# LOWFREQ_ROOT with a stub systemctl.
#
# Aufruf: sh remote-shell/ha/test_lowfreq_safe.sh
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
	printf 'verbosity=1\nuser_overlays=lowfreq\nextraargs=rootdev=UUID=1234\n' > "$ROOT/boot/armbianEnv.txt"
	printf 'MIN_SPEED=15625\nGOVERNOR=schedutil\n' > "$ROOT/etc/default/cpufrequtils"
	: > "$ROOT/boot/overlay-user/lowfreq.dtbo"
}

env_line() { grep "^$1=" "$ROOT/boot/armbianEnv.txt" 2>/dev/null || true; }

# --- 1. ohne Board: klarer Abbruch -----------------------------------------
out=$(LOWFREQ_ROOT="$WORK/leer" sh "$HERE/deploy_lowfreq_safe.sh" --dry-run 2>&1)
case "$out" in *"kein Board erkannt"*) ok "deploy bricht ohne Board ab" ;; *) bad "deploy Abbruch" ;; esac
out=$(LOWFREQ_ROOT="$WORK/leer" sh "$HERE/restore-stock.sh" --dry-run 2>&1)
case "$out" in *"kein Board erkannt"*) ok "restore bricht ohne Board ab" ;; *) bad "restore Abbruch" ;; esac

# --- 2. --help -------------------------------------------------------------
sh "$HERE/deploy_lowfreq_safe.sh" --help | grep -q 'Aufruf:' && ok "deploy --help" || bad "deploy --help"
sh "$HERE/restore-stock.sh" --help | grep -q 'Aufruf:' && ok "restore --help" || bad "restore --help"

# --- 3. deploy laesst user_overlays leer, setzt Marker ---------------------
new_root
LOWFREQ_ROOT="$ROOT" sh "$HERE/deploy_lowfreq_safe.sh" >/dev/null 2>&1
check "deploy: user_overlays leer" "$(env_line user_overlays)" "user_overlays="
[ -f "$ROOT/etc/lowfreq-test.pending" ] && ok "deploy: Marker gesetzt" || bad "deploy: Marker fehlt"
case "$(env_line extraargs)" in
	*performance*) ok "deploy: Boot-Governor gesetzt" ;;
	*) bad "deploy: Boot-Governor fehlt" ;;
esac
[ -f "$ROOT/etc/systemd/system/cpu-freq-guard.service" ] && ok "deploy: guard-Unit" || bad "deploy: guard-Unit fehlt"
[ -f "$ROOT/etc/systemd/system/lowfreq-test-hold.service" ] && ok "deploy: hold-Unit" || bad "deploy: hold-Unit fehlt"

# --- 4. Boot-Sequenz -------------------------------------------------------
HOLD="$ROOT/usr/local/sbin/lowfreq_test_hold.sh"
LOWFREQ_ROOT="$ROOT" sh "$HOLD" >/dev/null 2>&1
check "boot 1: Overlay scharf" "$(env_line user_overlays)" "user_overlays=lowfreq"
[ -f "$ROOT/etc/lowfreq-test.pending" ] && bad "boot 1: Marker blieb liegen" || ok "boot 1: Marker verbraucht"
LOWFREQ_ROOT="$ROOT" sh "$HOLD" >/dev/null 2>&1
check "boot 2: zurueck auf Werkswerte" "$(env_line user_overlays)" "user_overlays="
LOWFREQ_ROOT="$ROOT" sh "$HOLD" >/dev/null 2>&1
check "boot 3: idempotent" "$(env_line user_overlays)" "user_overlays="

# --- 5. promote ------------------------------------------------------------
new_root
LOWFREQ_ROOT="$ROOT" sh "$HERE/deploy_lowfreq_safe.sh" >/dev/null 2>&1
LOWFREQ_ROOT="$ROOT" sh "$HERE/deploy_lowfreq_safe.sh" --promote >/dev/null 2>&1
check "promote: Overlay bleibt" "$(env_line user_overlays)" "user_overlays=lowfreq"
[ -f "$ROOT/etc/lowfreq-test.pending" ] && bad "promote: Marker blieb" || ok "promote: Marker weg"
[ -f "$ROOT/etc/systemd/system/lowfreq-test-hold.service" ] && bad "promote: hold-Unit blieb" || ok "promote: hold-Unit weg"

# --- 6. restore-stock ------------------------------------------------------
new_root
LOWFREQ_ROOT="$ROOT" sh "$HERE/deploy_lowfreq_safe.sh" >/dev/null 2>&1
LOWFREQ_ROOT="$ROOT" sh "$HERE/restore-stock.sh" >/dev/null 2>&1
check "restore: user_overlays weg" "$(env_line user_overlays)" ""
case "$(env_line extraargs)" in
	*performance*) bad "restore: Boot-Governor blieb" ;;
	*) ok "restore: Boot-Governor weg" ;;
esac
[ -f "$ROOT/boot/overlay-user/lowfreq.dtbo" ] && bad "restore: dtbo blieb" || ok "restore: dtbo weg"
for f in etc/systemd/system/cpu-freq-guard.service etc/systemd/system/cpu-hotplug.service \
         etc/systemd/system/lowfreq-test-hold.service usr/local/sbin/cpu-freq-guard.sh \
         usr/local/sbin/cpu_hotplug.py etc/default/cpu-freq-guard; do
	[ -e "$ROOT/$f" ] && bad "restore: $f blieb" || ok "restore: $f weg"
done
check "restore: MIN_SPEED" "$(sed -n 's/^MIN_SPEED=//p' "$ROOT/etc/default/cpufrequtils")" "1000000"
check "restore: GOVERNOR" "$(sed -n 's/^GOVERNOR=//p' "$ROOT/etc/default/cpufrequtils")" "schedutil"

# --- 7. restore idempotent -------------------------------------------------
LOWFREQ_ROOT="$ROOT" sh "$HERE/restore-stock.sh" >/dev/null 2>&1 &&
	ok "restore: zweiter Lauf ok" || bad "restore: zweiter Lauf fehlgeschlagen"

echo
if [ "$FAIL" = 0 ]; then echo "ALLE TESTS OK"; exit 0; else echo "$FAIL Test(s) fehlgeschlagen"; exit 1; fi
