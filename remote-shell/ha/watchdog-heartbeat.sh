#!/bin/sh
# Board-Seite des externen Watchdogs: schreibt einen Heartbeat.
#
# Hintergrund: remote-shell/ha/WATCHDOG-GAP.md. Der interne HW-Watchdog hat
# einen PLL-Hang NICHT in 60 s rebootet. Dieser Timer erzeugt einen Heartbeat,
# den ein OFF-BOARD-Monitor (watchdog_monitor.py) beobachtet. Bleibt er aus,
# loest der Monitor einen unabhaengigen Aktor aus (Power-Cut/Reset).
#
# Bewusst minimal und ohne Abhaengigkeiten: reines sh, keine Uhr-Abhaengigkeit.
# Der Empfaenger stempelt die Zeit (siehe Monitor) - eine falsche Board-Uhr
# darf keinen Fehlalarm erzeugen.
#
# Format: <epoch> <boot_id> <seq>
#   boot_id kommt aus /proc/sys/kernel/random/boot_id (aendert sich je Boot)
#   seq ist ein monoton wachsender Zaehler (Datei), damit der Monitor einen
#   STEHENDEN Wert von einem NEUEN unterscheiden kann.
#
# Installation (Parent, am Board): siehe watchdog-heartbeat.service/.timer.
set -eu

OUT="${HEARTBEAT_OUT:-/run/board-heartbeat}"
SEQFILE="${HEARTBEAT_SEQ:-/run/board-heartbeat.seq}"

BOOT_ID="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)"

SEQ=0
[ -f "$SEQFILE" ] && SEQ="$(cat "$SEQFILE" 2>/dev/null || echo 0)"
case "$SEQ" in
    ''|*[!0-9]*) SEQ=0 ;;
esac
SEQ=$((SEQ + 1))

NOW="$(date +%s)"

# Atomar schreiben: erst temporaer, dann umbenennen, damit der Monitor nie eine
# halb geschriebene Zeile liest.
TMP="${OUT}.tmp.$$"
printf '%s %s %s\n' "$NOW" "$BOOT_ID" "$SEQ" > "$TMP"
mv -f "$TMP" "$OUT"
printf '%s\n' "$SEQ" > "$SEQFILE"

exit 0
