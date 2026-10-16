#!/bin/sh
# Installiert den CPU-Hotplug-Governor dauerhaft (systemd, enabled).
#
# Der Kernel hat keinen cpuidle-Treiber (DTB ohne idle-states), Kerne gehen im
# Leerlauf also nicht schlafen. Der Governor nimmt ungenutzte Kerne per Hotplug
# offline. cpu0 bleibt immer online.
#
# Aufruf: deploy_hotplug.sh
set -eu

HERE=$(dirname "$(readlink -f "$0")")

[ -f "$HERE/cpu_hotplug.py" ] || { echo "FEHLER: cpu_hotplug.py fehlt"; exit 1; }
[ -f "$HERE/cpu-hotplug.service" ] || { echo "FEHLER: cpu-hotplug.service fehlt"; exit 1; }

echo '== 1. Daemon installieren =='
install -m 0755 "$HERE/cpu_hotplug.py" /usr/local/sbin/cpu_hotplug.py
install -m 0755 "$HERE/lowfreq_set_floor.sh" /usr/local/sbin/lowfreq_set_floor.sh

echo '== 2. Dienst installieren =='
install -m 0644 "$HERE/cpu-hotplug.service" /etc/systemd/system/cpu-hotplug.service
systemctl daemon-reload

echo '== 3. aktivieren (reboot-fest) =='
systemctl enable cpu-hotplug.service
systemctl restart cpu-hotplug.service
sleep 2
systemctl is-active cpu-hotplug.service
systemctl is-enabled cpu-hotplug.service

echo '== 4. Zustand =='
echo "online: $(cat /sys/devices/system/cpu/online)"
echo "load:   $(cut -d' ' -f1 /proc/loadavg)"
journalctl -u cpu-hotplug.service -n 5 --no-pager 2>/dev/null || true
echo '== fertig =='
