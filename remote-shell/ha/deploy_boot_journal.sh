#!/bin/sh
# Installiert das persistente Boot-Journal (Reset-Zaehler + Grund) reboot-fest.
#
# Warum: Die Amlogic-Hardware liefert keinen Reset-Grund (watchdog bootstatus=0,
# pstore leer). Dieses Journal zaehlt Boots persistent und leitet den Grund aus
# Clean-Marker + Watchdog-Bootstatus + Panic-Spur ab.
#
# Aufruf: deploy_boot_journal.sh
set -eu

HERE=$(dirname "$(readlink -f "$0")")

for f in boot_journal.py boot_journal_lib.py boot-journal.service \
         boot-journal-heartbeat.service boot-journal-heartbeat.timer; do
  [ -f "$HERE/$f" ] || { echo "FEHLER: $f fehlt"; exit 1; }
done

echo '== 1. Programm installieren =='
install -m 0755 "$HERE/boot_journal.py" /usr/local/sbin/boot_journal.py
install -m 0644 "$HERE/boot_journal_lib.py" /usr/local/sbin/boot_journal_lib.py

echo '== 2. Units installieren =='
install -m 0644 "$HERE/boot-journal.service" /etc/systemd/system/boot-journal.service
install -m 0644 "$HERE/boot-journal-heartbeat.service" /etc/systemd/system/boot-journal-heartbeat.service
install -m 0644 "$HERE/boot-journal-heartbeat.timer" /etc/systemd/system/boot-journal-heartbeat.timer
systemctl daemon-reload

echo '== 3. aktivieren (reboot-fest) =='
systemctl enable boot-journal.service
systemctl enable boot-journal-heartbeat.timer
# start (nicht restart): der Dienst ist oneshot+RemainAfterExit, ein restart
# wuerde ExecStop=mark-clean ausloesen und den gerade erkannten Grund ueberschreiben.
systemctl start boot-journal.service
systemctl restart boot-journal-heartbeat.timer

# / ist ext4 mit commit=120: ohne sync kann ein Reset kurz nach dem Deploy die
# Schreibzugriffe verschlucken. Nach jedem Deploy flushen.
sync

echo '== 4. Zustand =='
systemctl is-active boot-journal.service
systemctl is-enabled boot-journal.service
systemctl is-active boot-journal-heartbeat.timer
systemctl is-enabled boot-journal-heartbeat.timer
/usr/bin/python3 /usr/local/sbin/boot_journal.py show || true
echo '== fertig =='
