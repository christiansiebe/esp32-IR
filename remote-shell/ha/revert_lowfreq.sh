#!/bin/sh
# Removes everything deploy_lowfreq.sh installed. Reboot afterwards.
set -u

echo '== 1. armbianEnv.txt bereinigen =='
sed -i '/^user_overlays=/d' /boot/armbianEnv.txt
sed -i 's/ cpufreq.default_governor=performance//' /boot/armbianEnv.txt
grep -nE 'user_overlays|extraargs' /boot/armbianEnv.txt

echo '== 2. overlay entfernen =='
rm -f /boot/overlay-user/lowfreq.dtbo

echo '== 3. guard service entfernen =='
systemctl disable cpu-freq-guard.service 2>/dev/null || true
rm -f /etc/systemd/system/cpu-freq-guard.service /usr/local/sbin/cpu-freq-guard.sh /etc/default/cpu-freq-guard

# Armbians MIN_SPEED auf den Werkswert zuruecksetzen (lowfreq_set_floor.sh hat
# ihn auf die tiefe Rate gezogen, sonst bleibt die Untergrenze klein).
sed -i 's/^MIN_SPEED=.*/MIN_SPEED=1000000/' /etc/default/cpufrequtils 2>/dev/null || true
for p in /sys/devices/system/cpu/cpufreq/policy*; do
	[ -w "$p/scaling_min_freq" ] && echo 1000000 > "$p/scaling_min_freq" 2>/dev/null || true
done

# Hotplug-Governor entfernen und alle Kerne wieder online nehmen.
systemctl disable --now cpu-hotplug.service 2>/dev/null || true
rm -f /etc/systemd/system/cpu-hotplug.service /usr/local/sbin/cpu_hotplug.py
for c in /sys/devices/system/cpu/cpu[0-9]*; do
	[ -w "$c/online" ] && echo 1 > "$c/online" 2>/dev/null || true
done
systemctl daemon-reload 2>/dev/null || true

echo '== fertig: Reboot noetig =='
