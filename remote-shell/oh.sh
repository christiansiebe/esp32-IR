#!/usr/bin/env bash
# oh - Text in normaler Sprache an den lokalen OpenHands-Agenten auf dem Odroid.
#
#   oh Fuehre hostname aus und gib das Ergebnis zurueck
#   oh -n starte eine frische Unterhaltung
#   oh -w /root arbeite in /root
#   oh -s arbeit ...   eigene Session "arbeit" (parallel zu anderen moeglich)
#
# Text darf OHNE Anfuehrungszeichen getippt werden (alle Woerter werden
# zusammengefuegt). Gibt die Antwort des Agenten und die benutzten
# Terminal-Befehle aus. Der Agent laeuft auf http://127.0.0.1:8000.
set -euo pipefail

OH_BASE="${OH_BASE:-http://127.0.0.1:8000}"
OH_AID="${OH_AID:-ef81fbef-81ea-4940-9a20-ab210add363c}"
OH_SESS=""
OH_NEW=0
OH_WD=/root

while getopts "nw:s:" _o; do
  case "$_o" in
    n) OH_NEW=1 ;;
    w) OH_WD="$OPTARG" ;;
    s) OH_SESS="$OPTARG" ;;
    *) echo "Nutzung: oh [-n] [-w ARBEITSVERZEICHNIS] [-s SESSION] Aufgabe in Worten" >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
TEXT="$*"
[ -n "$TEXT" ] || { echo "Aufgabentext fehlt.  Beispiel: oh hostname und pwd ausfuehren" >&2; exit 2; }

# Jede Session hat ihre eigene Conversation -> parallele Chats moeglich.
if [ -n "$OH_SESS" ]; then
  OH_STATE="${OH_STATE:-/root/.oh-conv-$OH_SESS}"
else
  OH_STATE="${OH_STATE:-/root/.oh-last-conv}"
fi

OH_BASE="$OH_BASE" OH_AID="$OH_AID" OH_STATE="$OH_STATE" \
OH_NEW="$OH_NEW" OH_WD="$OH_WD" OH_TEXT="$TEXT" python3 -u - <<'PY'
import json, os, time, urllib.request, urllib.error

B = os.environ["OH_BASE"]; AID = os.environ["OH_AID"]; ST = os.environ["OH_STATE"]
NEW = os.environ["OH_NEW"] == "1"; WD = os.environ["OH_WD"]; TXT = os.environ["OH_TEXT"]

def req(m, p, b=None, t=180):
    d = json.dumps(b).encode() if b is not None else None
    r = urllib.request.Request(B + p, data=d, method=m)
    r.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(r, timeout=t) as x:
            return x.status, x.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return -1, str(e)

def text_of(msg):
    if not isinstance(msg, dict):
        return ""
    parts = msg.get("content") or []
    return "".join(c.get("text", "") for c in parts if isinstance(c, dict)).strip()

cid = None
if not NEW and os.path.exists(ST):
    cid = open(ST).read().strip() or None
if cid and req("GET", f"/api/conversations/{cid}")[0] != 200:
    cid = None

if not cid:
    st, o = req("POST", "/api/conversations",
                {"agent_profile_id": AID,
                 "workspace": {"kind": "LocalWorkspace", "working_dir": WD}})
    if st != 201:
        print("Fehler beim Anlegen der Conversation:", st, o[:300]); raise SystemExit(1)
    cid = json.loads(o)["id"]
    open(ST, "w").write(cid)
    print(f"neue Conversation: {cid}  (cwd {WD})")
else:
    print(f"Conversation: {cid}")

st, ev = req("GET", f"/api/conversations/{cid}/events/search?limit=100")
seen = len(json.loads(ev).get("items", [])) if st == 200 else 0

st, o = req("POST", f"/api/conversations/{cid}/events",
            {"role": "user", "content": [{"type": "text", "text": TXT}], "run": True})
if st != 200:
    print("Senden fehlgeschlagen:", st, o[:300]); raise SystemExit(1)
print("gesendet. warte auf Antwort ...\n")

got_answer = False
idle_rounds = 0
for _ in range(80):
    time.sleep(3)
    st, ev = req("GET", f"/api/conversations/{cid}/events/search?limit=100")
    items = json.loads(ev).get("items", []) if st == 200 else []
    for e in items[seen:]:
        k = e.get("kind")
        if k == "ActionEvent":
            tn = e.get("tool_name"); a = e.get("action") or {}
            if tn == "terminal":
                print("  $ " + str(a.get("command", "")))
            elif tn and tn != "finish":
                print(f"  [{tn}] " + json.dumps(a)[:120])
        elif k == "ObservationEvent" and e.get("tool_name") == "terminal":
            ob = e.get("observation") or {}
            txt = text_of(ob)
            if txt:
                print(f"  -> exit {ob.get('exit_code')}: {txt[:500].rstrip()}")
        elif k == "MessageEvent" and e.get("source") == "agent":
            m = text_of(e.get("llm_message") or {})
            if m:
                print("\nAgent:", m)
                got_answer = True
        elif k == "ErrorEvent":
            print("  ERROR", json.dumps(e)[:250])
    seen = len(items)
    st2, cj = req("GET", f"/api/conversations/{cid}")
    status = json.loads(cj).get("execution_status") if st2 == 200 else "?"
    # "idle" tritt direkt nach dem Senden kurz auf, bevor der Agent startet:
    # erst beenden, wenn der Agent wirklich geantwortet hat (oder Fehler/Stop).
    if status in ("finished", "error", "stopped") and got_answer:
        break
    if status == "idle":
        idle_rounds += 1
        if got_answer and idle_rounds >= 2:
            break
    else:
        idle_rounds = 0
print(f"\n(Conversation {cid})")
PY
