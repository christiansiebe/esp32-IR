#!/bin/sh
# Verbessert die Kernel-Absturzaufzeichnung auf dem Odroid N2 (Armbian).
#
# Warum: der Oops vom 2026-10-20 (ESR 0x86000006) hat keine Spur hinterlassen.
# Zwei Luecken werden hier geschlossen:
#   1. Ein Oops ist kein Panic -> panic_on_oops=1 (Sysctl + Cmdline). Erst dann
#      schreiben ramoops und panic=10 ueberhaupt etwas.
#   2. journald syncht zu langsam -> SyncIntervalSec=1s + groesserer Deckel.
# Optional (nur mit Argumenten): netconsole, der einzige Weg, auch einen HARTEN
# Hang sichtbar zu machen.
#
# Idempotent: mehrfaches Ausfuehren aendert nichts. Reversibel: revert_crash_capture.sh.
#
# Board-Zugriff erfolgt bewusst NICHT aus diesem Skript heraus — es wird vom
# Parent bewusst und sequenziell ueber den geteilten Kanal gefahren.
set -eu

HERE=$(dirname "$(readlink -f "$0")")
SRC="$HERE/crash_capture.py"

echo '== 1. Sysctl-Drop-in (Oops -> Panic) =='
python3 "$SRC" render-sysctl > /etc/sysctl.d/99-crash-capture.conf
cat /etc/sysctl.d/99-crash-capture.conf

echo '== 2. journald-Drop-in (frueher flushen) =='
mkdir -p /etc/systemd/journald.conf.d
python3 "$SRC" render-journald > /etc/systemd/journald.conf.d/99-crash-capture.conf
cat /etc/systemd/journald.conf.d/99-crash-capture.conf

echo '== 3. Kernel-Cmdline (extraargs) =='
CUR=$(sed -n 's/^extraargs=//p' /boot/armbianEnv.txt | head -1)
NEW=$(python3 "$SRC" render-extraargs "$CUR")
if grep -q '^extraargs=' /boot/armbianEnv.txt; then
  sed -i "s|^extraargs=.*$|extraargs=$NEW|" /boot/armbianEnv.txt
else
  printf 'extraargs=%s\n' "$NEW" >> /boot/armbianEnv.txt
fi
grep -n '^extraargs=' /boot/armbianEnv.txt

echo '== 4. netconsole (optional: braucht einen zweiten Host) =='
if [ "$#" -ge 3 ]; then
  # Aufruf: deploy_crash_capture.sh <src-ip> <iface> <dst-ip> <dst-mac>
  python3 "$SRC" render-netconsole "$1" "$2" "$3" "${4:-}" \
    > /etc/modprobe.d/netconsole.conf
  echo 'netconsole' > /etc/modules-load.d/netconsole.conf
  cat /etc/modprobe.d/netconsole.conf
else
  echo 'uebersprungen (kein Empfaenger angegeben) — Aufruf mit: <src-ip> <iface> <dst-ip> [dst-mac]'
fi

echo '== 5. ramoops vorhanden? (PR #45) =='
if [ -d /sys/fs/pstore ]; then
  echo '/sys/fs/pstore vorhanden'
fi
if ! lsmod | grep -q '^ramoops'; then
  echo 'HINWEIS: ramoops ist NICHT geladen -> deploy_ramoops.sh (PR #45) nachziehen.'
fi

echo '== 6. Sofort anwenden (Sysctl) =='
sysctl --system >/dev/null 2>&1 || true
systemctl restart systemd-journald 2>/dev/null || true

# / ist ext4 mit commit=120: ohne sync kann ein Reset kurz nach dem Deploy die
# Schreibzugriffe verschlucken.
sync

echo '== fertig — Cmdline-Aenderungen greifen erst nach einem Reboot =='
