#!/bin/sh
# Installiert die OFF-BOARD-Seite des externen Watchdogs.
#
# Nur die Beobachter-Seite: Monitor + Aktor-Wrapper + systemd-Service. Der
# Emitter (watchdog-heartbeat.*) gehoert auf das BOARD und wird hier absichtlich
# NICHT installiert (dafuer deploy_heartbeat.sh auf dem Board).
#
# Siehe remote-shell/ha/WATCHDOG-ACTOR.md.
#
# WICHTIG: Der Aktor wird hier nur INSTALLIERT, nicht scharf geschaltet. Der
# Wrapper laeuft per Default im Trockenlauf. Scharf erst mit
# WATCHDOG_POWERCUT_ARMED=1 in einem Drop-in, nach Trockenlauf + Testdose.
#
# Aufruf: deploy_watchdog_actor.sh [--armed]
#   --armed  schreibt ein Drop-in, das WATCHDOG_POWERCUT_ARMED=1 setzt
set -eu

HERE=$(dirname "$(readlink -f "$0")")
ARMED=0
for arg in "$@"; do
    [ "$arg" = "--armed" ] && ARMED=1
done

for f in watchdog_monitor.py watchdog_actor.py watchdog_powercut.sh \
         watchdog-monitor.service WATCHDOG-ACTOR.md WATCHDOG-GAP.md; do
    [ -f "$HERE/$f" ] || { echo "FEHLER: $f fehlt"; exit 1; }
done

echo '== 1. Beobachter + Aktor installieren =='
install -m 0755 "$HERE/watchdog_monitor.py"  /usr/local/sbin/watchdog_monitor.py
install -m 0755 "$HERE/watchdog_actor.py"    /usr/local/sbin/watchdog_actor.py
install -m 0755 "$HERE/watchdog_powercut.sh" /usr/local/sbin/watchdog_powercut.sh
install -d -m 0755 /usr/local/share/remote-shell
install -m 0644 "$HERE/WATCHDOG-ACTOR.md" /usr/local/share/remote-shell/WATCHDOG-ACTOR.md
install -m 0644 "$HERE/WATCHDOG-GAP.md"   /usr/local/share/remote-shell/WATCHDOG-GAP.md
install -d -m 0755 /var/lib/watchdog

echo '== 2. systemd-Service installieren =='
install -m 0644 "$HERE/watchdog-monitor.service" /etc/systemd/system/watchdog-monitor.service
systemctl daemon-reload

if [ "$ARMED" = "1" ]; then
    echo '== 2b. SCHARF schalten (WATCHDOG_POWERCUT_ARMED=1) =='
    install -d -m 0755 /etc/systemd/system/watchdog-monitor.service.d
    cat > /etc/systemd/system/watchdog-monitor.service.d/armed.conf <<'CONF'
[Service]
Environment=WATCHDOG_POWERCUT_ARMED=1
CONF
    systemctl daemon-reload
    echo 'WARNUNG: der Aktor ist jetzt scharf. Trockenlauf vorher pruefen!'
else
    echo '== 2b. Trockenlauf (Default): nichts wird geschaltet =='
fi

echo '== 3. Service aktivieren (reboot-fest) =='
systemctl enable --now watchdog-monitor.service
sleep 2
systemctl is-active watchdog-monitor.service
systemctl is-enabled watchdog-monitor.service

echo '== 4. Zustand =='
systemctl status watchdog-monitor.service --no-pager 2>/dev/null | head -12 || true
echo '== fertig =='
echo
echo 'Naechster Schritt: Board-Heartbeat-Transport pruefen (WATCHDOG-ACTOR.md 2.0),'
echo 'dann Wrapper mit markierter Testdose scharf testen, dann --armed.'
