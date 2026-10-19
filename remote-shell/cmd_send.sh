#!/bin/bash
# Sandbox-Seite: Befehl an den Odroid-Agenten senden (Datei-Kanal).
#
#   tools/remote-shell/cmd_send.sh "uname -a"
#   tools/remote-shell/cmd_send.sh "btrfs subvolume list /" 60
#
# Der Server legt den Befehl in die Queue; der Agent auf dem Odroid zeigt ihn
# an und fuehrt ihn nach lokaler Bestaetigung aus. Die Ausgabe wird LIVE
# angezeigt (Zeilen/Fortschrittsbalken), das Endergebnis kommt aus results/.
set -euo pipefail

CMD="${1:?Befehl fehlt}"
TIMEOUT="${2:-1800}"

# Auto-Wrap: die Odroid-Allowlist erlaubt `bash .*` (Agent-Autonomie). Deshalb
# wird ein blanker Befehl hier automatisch in `bash -lc '...'` gepackt, damit
# man nicht bei jedem Aufruf selbst wrappen muss. Schon gewrappte Befehle
# (bash/python3) und die Datei-Verben @read/@write bleiben unveraendert; mit dem
# Praefix `raw:` laesst sich das Wrapping gezielt abschalten.
case "$CMD" in
  bash\ *|python3\ *|@read*|@write*) : ;;
  raw:*) CMD="${CMD#raw:}" ;;
  *)     CMD="bash -lc $(printf '%q' "$CMD")" ;;
esac

BASE="${CMD_BASE:-/tmp/cmdchannel}"
QDIR="$BASE/queue"
RDIR="$BASE/results"
LDIR="$BASE/live"
mkdir -p "$QDIR" "$RDIR" "$LDIR"

# Server selbstheilend sicherstellen: laeuft er nicht (z.B. nach einem
# Sandbox-Reset), startet ihn der Supervisor. Idempotent und billig, damit
# JEDER Chat in dieser Sandbox den Kanal ohne Vorkenntnisse nutzen kann.
SUPERVISE="$(dirname "$0")/supervise.py"
if [ -f "$SUPERVISE" ]; then
  python3 "$SUPERVISE" --status >/dev/null 2>&1 || python3 "$SUPERVISE" >/dev/null 2>&1 || true
fi

ID="$(date +%s)-$$"
printf '%s' "$CMD" > "$QDIR/$ID.cmd"
echo "gesendet: id=$ID  (warte auf Bestaetigung am Odroid ...)"

DEADLINE=$(( $(date +%s) + TIMEOUT ))
SHOWN=0
while [ ! -f "$RDIR/$ID.json" ]; do
  if [ -f "$LDIR/$ID.log" ]; then
    SIZE=$(wc -c < "$LDIR/$ID.log")
    if [ "$SIZE" -gt "$SHOWN" ]; then
      tail -c +$((SHOWN + 1)) "$LDIR/$ID.log"
      SHOWN=$SIZE
    fi
  fi
  if [ "$(date +%s)" -ge "$DEADLINE" ]; then
    echo "TIMEOUT: keine Antwort innerhalb ${TIMEOUT}s"
    exit 2
  fi
  sleep 1
done

# Rest der Live-Ausgabe (falls nach dem letzten Poll noch etwas kam)
if [ -f "$LDIR/$ID.log" ]; then
  SIZE=$(wc -c < "$LDIR/$ID.log")
  [ "$SIZE" -gt "$SHOWN" ] && tail -c +$((SHOWN + 1)) "$LDIR/$ID.log"
fi

echo
echo "--- exit code ---"
cat "$RDIR/$ID.code" 2>/dev/null || true
echo
echo "--- ausgabe (vollstaendig) ---"
cat "$RDIR/$ID.out" 2>/dev/null || true
