#!/bin/sh
# Macht deploy_crash_capture.sh rueckgaengig: entfernt die Drop-ins und nimmt
# die Crash-Token wieder aus der Kernel-Cmdline. ramoops bleibt unangetastet
# (dafuer gibt es revert_ramoops.sh).
set -eu

echo '== 1. Sysctl-Drop-in entfernen =='
rm -f /etc/sysctl.d/99-crash-capture.conf

echo '== 2. journald-Drop-in entfernen =='
rm -f /etc/systemd/journald.conf.d/99-crash-capture.conf
rmdir /etc/systemd/journald.conf.d 2>/dev/null || true

echo '== 3. Kernel-Cmdline bereinigen =='
if [ -f /boot/armbianEnv.txt ]; then
  CUR=$(sed -n 's/^extraargs=//p' /boot/armbianEnv.txt | head -1)
  NEW=$(printf '%s' "$CUR" | tr ' ' '\n' \
        | grep -vE '^(panic_on_oops=1|hung_task_panic=1)$' \
        | paste -sd' ' -)
  sed -i "s|^extraargs=.*$|extraargs=$NEW|" /boot/armbianEnv.txt
  grep -n '^extraargs=' /boot/armbianEnv.txt
fi

echo '== 4. netconsole entfernen (falls vorhanden) =='
rm -f /etc/modprobe.d/netconsole.conf /etc/modules-load.d/netconsole.conf

echo '== 5. anwenden =='
sysctl --system >/dev/null 2>&1 || true
systemctl restart systemd-journald 2>/dev/null || true
sync

echo '== fertig — Cmdline-Aenderung greift erst nach einem Reboot =='
