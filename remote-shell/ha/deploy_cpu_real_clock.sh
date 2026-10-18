#!/bin/sh
# Installiert cpu-real-clock dauerhaft auf dem Odroid.
#
# Was es macht:
#   - /usr/local/bin/cpu-real-clock   (Daemon: macht scaling_cur_freq ehrlich)
#   - /etc/systemd/system/cpu-real-clock.service
#   - aktiviert + startet den Dienst (reboot-fest)
#
# Warum: der Amlogic-cpufreq-dt-Treiber meldet in scaling_cur_freq den OPP, den
# er zu setzen GLAUBT. Ein manueller PLL-Overclock (HHI 0xff63c000+0x2f4 big /
# +0x380 little) ist fuer den Treiber unsichtbar -> alle Anzeigen (cpu-live,
# cpu-volt-line, htopv, htop, HA) zeigen 2400 statt echt 2472. Dieser Daemon
# ueberlagert scaling_cur_freq per bind-Mount mit dem echten Takt.
#
# Aufruf: deploy_cpu_real_clock.sh
set -eu

HERE=$(dirname "$(readlink -f "$0")")
[ -f "$HERE/cpu_real_clock.py" ] || { echo "FEHLER: $HERE/cpu_real_clock.py fehlt" >&2; exit 1; }
[ -f "$HERE/cpu-real-clock.service" ] || { echo "FEHLER: $HERE/cpu-real-clock.service fehlt" >&2; exit 1; }

echo '== 1. Daemon installieren =='
install -m 0755 "$HERE/cpu_real_clock.py" /usr/local/bin/cpu-real-clock
install -m 0644 "$HERE/cpu-real-clock.service" /etc/systemd/system/cpu-real-clock.service

echo '== 2. Dienst aktivieren =='
systemctl daemon-reload
systemctl enable --now cpu-real-clock.service

sleep 2
echo '== 3. Zustand =='
systemctl --no-pager --lines=0 status cpu-real-clock.service | head -6 || true
echo '-- scaling_cur_freq (echter Takt) --'
for p in policy0 policy2; do
        f=/sys/devices/system/cpu/cpufreq/$p/scaling_cur_freq
        printf '  %-8s %s\n' "$p" "$(cat "$f" 2>/dev/null || echo '(offline)')"
done
echo '-- Anzeige --'
/usr/local/bin/cpu-volt-line 2>/dev/null || true
echo
echo '== fertig - Deaktivieren: systemctl disable --now cpu-real-clock =='
