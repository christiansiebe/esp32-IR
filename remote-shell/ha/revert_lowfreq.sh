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
systemctl daemon-reload

echo '== fertig: Reboot noetig =='
