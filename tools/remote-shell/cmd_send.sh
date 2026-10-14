#!/bin/bash
# Sandbox-Seite: Befehl an den Odroid-Agenten senden (Datei-Kanal).
#
#   tools/remote-shell/cmd_send.sh "uname -a"
#   tools/remote-shell/cmd_send.sh "btrfs subvolume list /" 60
#
# Der Server (sandbox_term_server.py) legt den Befehl in die Queue; der Agent
# auf dem Odroid zeigt ihn an und fuehrt ihn nach lokaler Bestaetigung aus.
# Ergebnis: /tmp/cmdchannel/results/<id>.out (+ .json)
set -euo pipefail

CMD="${1:?Befehl fehlt}"
TIMEOUT="${2:-120}"
BASE="${CMD_BASE:-/tmp/cmdchannel}"
QDIR="$BASE/queue"
RDIR="$BASE/results"
mkdir -p "$QDIR" "$RDIR"

ID="$(date +%s)-$$"
printf '%s' "$CMD" > "$QDIR/$ID.cmd"
echo "gesendet: id=$ID  (warte auf Bestaetigung am Odroid ...)"

DEADLINE=$(( $(date +%s) + TIMEOUT ))
while [ ! -f "$RDIR/$ID.json" ]; do
  if [ "$(date +%s)" -ge "$DEADLINE" ]; then
    echo "TIMEOUT: keine Antwort innerhalb ${TIMEOUT}s"
    exit 2
  fi
  sleep 1
done

echo "--- exit code ---"
cat "$RDIR/$ID.code" 2>/dev/null || true
echo
echo "--- ausgabe ---"
cat "$RDIR/$ID.out" 2>/dev/null || true
