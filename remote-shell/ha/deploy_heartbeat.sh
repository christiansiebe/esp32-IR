#!/bin/sh
# Installiert den Heartbeat-Emitter (Board-Seite des externen Watchdogs).
#
# Nur die BOARD-Seite: Emitter + systemd-Timer. Der Monitor
# (watchdog_monitor.py) laeuft OFF-BOARD und ist hier absichtlich NICHT
# installiert - er gehoert auf den Beobachter (Sandbox-Kanal / MCU).
#
# Siehe remote-shell/ha/WATCHDOG-GAP.md.
#
# Aufruf: deploy_heartbeat.sh
set -eu

HERE=$(dirname "$(readlink -f "$0")")

[ -f "$HERE/watchdog-heartbeat.sh" ] || { echo "FEHLER: watchdog-heartbeat.sh fehlt"; exit 1; }
[ -f "$HERE/watchdog-heartbeat.service" ] || { echo "FEHLER: watchdog-heartbeat.service fehlt"; exit 1; }
[ -f "$HERE/watchdog-heartbeat.timer" ] || { echo "FEHLER: watchdog-heartbeat.timer fehlt"; exit 1; }

echo '== 1. Emitter installieren =='
install -m 0755 "$HERE/watchdog-heartbeat.sh" /usr/local/sbin/watchdog-heartbeat.sh
install -d -m 0755 /usr/local/share/remote-shell
install -m 0644 "$HERE/WATCHDOG-GAP.md" /usr/local/share/remote-shell/WATCHDOG-GAP.md

echo '== 2. Units installieren =='
install -m 0644 "$HERE/watchdog-heartbeat.service" /etc/systemd/system/watchdog-heartbeat.service
install -m 0644 "$HERE/watchdog-heartbeat.timer" /etc/systemd/system/watchdog-heartbeat.timer
systemctl daemon-reload

echo '== 3. Timer aktivieren (reboot-fest) =='
systemctl enable --now watchdog-heartbeat.timer
sleep 2
systemctl is-active watchdog-heartbeat.timer
systemctl is-enabled watchdog-heartbeat.timer

echo '== 4. Zustand =='
systemctl list-timers watchdog-heartbeat.timer --no-pager 2>/dev/null || true
echo "heartbeat: $(cat /run/board-heartbeat 2>/dev/null || echo '<noch nicht geschrieben>')"
echo '== fertig =='
echo
echo 'Naechster Schritt (Parent, OFF-BOARD): watchdog_monitor.py auf dem'
echo 'Beobachter starten, z. B.'
echo "  python3 watchdog_monitor.py --heartbeat <pfad> --reboot-cmd '<power-cut>'"
