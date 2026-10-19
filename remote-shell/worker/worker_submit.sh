#!/bin/bash
# Parent-Seite: Task an die Odroid-Worker-Queue geben und Ergebnis abholen.
#
#   worker_submit.sh "Analysiere X und schreibe Y"        # Task einstellen
#   worker_submit.sh --wait 600 "..."                     # einstellen + warten
#   worker_submit.sh --list                               # offene/fertige Tasks
#   worker_submit.sh --result <id>                        # Ergebnis holen
#
# Der Task landet in /root/worker/queue auf dem Odroid. Ein freier Worker
# (odroid-worker@1 / @2) greift ihn auf und arbeitet ihn mit `oh -s workerN`
# ab -> lokale Conversation, KEIN Cloud-Guthaben.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# cmd_send.sh liegt eine Ebene hoeher (remote-shell/), ausser im worker/-Ordner.
if [ -f "$HERE/cmd_send.sh" ]; then SEND="$HERE/cmd_send.sh"; else SEND="$HERE/../cmd_send.sh"; fi
WDIR="${WORKER_DIR:-/root/worker}"

emit() { # Task-JSON base64-sicher auf den Odroid legen
  local text="$1" id="$2" by="$3"
  local payload
  payload=$(python3 -c '
import json,sys,os
print(json.dumps({"text":sys.argv[1],"submitted_by":sys.argv[2],
                  "ts":__import__("time").strftime("%Y-%m-%dT%H:%M:%SZ",__import__("time").gmtime())}))
' "$text" "$by")
  local b64
  b64=$(printf '%s' "$payload" | base64 -w0)
  bash "$SEND" "mkdir -p $WDIR/queue; echo $b64 | base64 -d > $WDIR/queue/$id.task; echo eingestellt:$id" 60
}

case "${1:-}" in
  --list)
    bash "$SEND" "echo '--- queue ---'; ls $WDIR/queue 2>/dev/null; echo '--- running ---'; ls $WDIR/running 2>/dev/null; echo '--- results ---'; ls $WDIR/results 2>/dev/null" 60
    exit 0 ;;
  --result)
    ID="${2:?Task-ID fehlt}"
    bash "$SEND" "cat $WDIR/results/$ID.json 2>/dev/null || echo '(noch kein Ergebnis)'" 60
    exit 0 ;;
  --wait)
    WAIT="${2:?Sekunden fehlt}"; shift 2 ;;
  *) WAIT=0 ;;
esac

TEXT="${1:?Aufgabentext fehlt}"
BY="${WORKER_SUBMITTED_BY:-parent}"
ID="$(date +%s)-$$"

if [ "$WAIT" -eq 0 ]; then
  emit "$TEXT" "$ID" "$BY"
  echo "Task-ID: $ID   (Ergebnis: $0 --result $ID)"
  exit 0
fi

emit "$TEXT" "$ID" "$BY"
echo "Task-ID: $ID  (warte max ${WAIT}s)"
DEADLINE=$(( $(date +%s) + WAIT ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  R="$(bash "$SEND" "cat $WDIR/results/$ID.json 2>/dev/null || echo PENDING" 60 || echo PENDING)"
  # cmd_send.sh umrahmt die Ausgabe mit Text -> nur das JSON-Objekt extrahieren.
  PARSED="$(printf '%s' "$R" | python3 -c '
import sys, json
s = sys.stdin.read()
i = s.find("{")
if i < 0: print("PENDING"); raise SystemExit
try:
    d = json.loads(s[i:])
except ValueError:
    print("PENDING"); raise SystemExit
print("status:", d.get("status"), "worker:", d.get("worker"))
print(d.get("output",""))' 2>/dev/null || echo PENDING)"
  if [ "$PARSED" != "PENDING" ]; then
    printf '%s\n' "$PARSED"
    exit 0
  fi
  sleep 10
done
echo "TIMEOUT: Task $ID noch nicht fertig."
