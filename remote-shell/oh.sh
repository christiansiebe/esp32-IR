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
#
# Resilienz: bricht der LLM-Aufruf transient ab (ConversationErrorEvent mit
# classification.retryable=true, z.B. "Missing credentials" am LiteLLM-Proxy),
# wiederholt `oh` den Auftrag automatisch mit Backoff. Eine Conversation, die
# mit einem Schluessel-Snapshot aus der Zeit VOR einer Rotation angelegt wurde,
# bleibt dauerhaft kaputt; deshalb wird nach einem solchen Fehler auf eine
# frische Conversation ausgewichen (Selbstheilung). Exit-Code != 0 bei Fehler,
# damit der Worker den Task als fehlgeschlagen/retryable erkennt.
#
# Stellschrauben (Env):
#   OH_RETRIES=3        zusaetzliche Versuche nach einem transienten Fehler
#   OH_RETRY_BASE=8     Basis-Wartezeit in Sekunden (verdoppelt sich je Versuch)
#   OH_FRESH_ON_ERROR=1 bei transientem Fehler auf frische Conversation wechseln
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
OH_NEW="$OH_NEW" OH_WD="$OH_WD" OH_TEXT="$TEXT" \
OH_RETRIES="${OH_RETRIES:-3}" OH_RETRY_BASE="${OH_RETRY_BASE:-8}" \
OH_FRESH_ON_ERROR="${OH_FRESH_ON_ERROR:-1}" python3 -u - <<'PY'
import json, os, time, urllib.request, urllib.error

B = os.environ["OH_BASE"]; AID = os.environ["OH_AID"]; ST = os.environ["OH_STATE"]
NEW = os.environ["OH_NEW"] == "1"; WD = os.environ["OH_WD"]; TXT = os.environ["OH_TEXT"]
RETRIES = int(os.environ.get("OH_RETRIES", "3") or "3")
RETRY_BASE = float(os.environ.get("OH_RETRY_BASE", "8") or "8")
FRESH_ON_ERROR = os.environ.get("OH_FRESH_ON_ERROR", "1") == "1"

# Fehlercodes/Detail-Muster, die einen erneuten Versuch rechtfertigen.
RETRYABLE_CODES = {
    "LLMServiceUnavailableError", "LLMError", "InternalServerError",
    "RateLimitError", "ServiceUnavailableError", "APIConnectionError",
    "TimeoutError", "LLMTimeoutError",
}
RETRYABLE_HINTS = (
    "missing credentials", "litellm_proxyexception", "internalservererror",
    "rate limit", "temporarily unavailable", "timeout", "connection reset",
    "service unavailable", "502", "503", "504",
)
# "Missing credentials" bedeutet: der Schluessel dieser Conversation ist stale
# (angelegt vor einer Rotation) -> nur eine FRISCHE Conversation hilft.
STALE_KEY_HINTS = ("missing credentials", "litellm_proxyexception")


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


def recent_events(cid, limit=100):
    """Die letzten `limit` Events, aufsteigend (aelteste zuerst).

    events/search liefert max. 100 Events pro Seite und standardmaessig die
    AELTESTEN. Bei Conversations mit >100 Events wuerde ein Zaehler-Offset
    (len(items)) nie neue Events sehen. Deshalb fragen wir absteigend ab und
    drehen die Liste um -> die aktuellsten Events sind immer dabei.
    """
    st, ev = req("GET", f"/api/conversations/{cid}/events/search"
                        f"?limit={limit}&sort_order=TIMESTAMP_DESC")
    if st != 200:
        return []
    items = json.loads(ev).get("items", [])
    return list(reversed(items))


def classify_error(e):
    """(retryable: bool, stale_key: bool, label: str) fuer ein Fehler-Event."""
    code = str(e.get("code") or "")
    detail = str(e.get("detail") or "").lower()
    cls = e.get("classification") or {}
    retryable = bool(cls.get("retryable")) or code in RETRYABLE_CODES \
        or any(h in detail for h in RETRYABLE_HINTS)
    stale = any(h in detail for h in STALE_KEY_HINTS)
    return retryable, stale, (code or "ErrorEvent")


def new_conversation():
    st, o = req("POST", "/api/conversations",
                {"agent_profile_id": AID,
                 "workspace": {"kind": "LocalWorkspace", "working_dir": WD}})
    if st != 201:
        print("Fehler beim Anlegen der Conversation:", st, o[:300])
        return None
    cid = json.loads(o)["id"]
    open(ST, "w").write(cid)
    print(f"neue Conversation: {cid}  (cwd {WD})")
    return cid


def existing_conversation():
    if NEW or not os.path.exists(ST):
        return None
    cid = open(ST).read().strip() or None
    if cid and req("GET", f"/api/conversations/{cid}")[0] != 200:
        return None
    return cid


def send_and_wait(cid):
    """Text an cid senden und auf Antwort/Fehler warten.

    Rueckgabe: (result, payload)
      result: 'answer' | 'error' | 'send_failed' | 'timeout'
      payload: Antworttext bzw. (retryable, stale, label) bzw. Fehlertext.

    Neue Events werden ueber die Event-ID-Menge erkannt (nicht ueber einen
    Zaehler-Offset) -> funktioniert auch bei Conversations mit >100 Events.
    """
    seen_ids = {e.get("id") for e in recent_events(cid)}

    st, o = req("POST", f"/api/conversations/{cid}/events",
                {"role": "user", "content": [{"type": "text", "text": TXT}], "run": True})
    if st != 200:
        return "send_failed", f"{st} {o[:300]}"
    print("gesendet. warte auf Antwort ...\n")

    got_answer = False
    idle_rounds = 0
    retry_info = None
    for _ in range(80):
        time.sleep(3)
        items = recent_events(cid)
        fresh = [e for e in items if e.get("id") not in seen_ids]
        for e in fresh:
            k = e.get("kind")
            if k == "ActionEvent":
                tn = e.get("tool_name"); a = e.get("action") or {}
                if tn == "terminal":
                    print("  $ " + str(a.get("command", "")))
                elif tn == "finish":
                    msg = str(a.get("message", "")).strip()
                    if msg:
                        print("\nAgent:", msg)
                        got_answer = True
                elif tn:
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
            elif k in ("ErrorEvent", "ConversationErrorEvent"):
                code = e.get("code"); detail = e.get("detail")
                print("  ERROR", code, str(detail)[:250])
                retry_info = classify_error(e)
        seen_ids.update(e.get("id") for e in items)
        # Fehler-Event ist terminal: nicht auf das langsame Auslaufen des
        # Status warten (der Lauf wird nie mehr erfolgreich).
        if retry_info is not None:
            return "error", retry_info
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
    return ("answer" if got_answer else "timeout"), None


cid = existing_conversation()
if not cid:
    cid = new_conversation()
    if not cid:
        raise SystemExit(1)
else:
    print(f"Conversation: {cid}")

attempt = 0
result, payload = send_and_wait(cid)
while result == "error" and attempt < RETRIES:
    retryable, stale, label = payload
    if not retryable:
        break
    attempt += 1
    wait = RETRY_BASE * (2 ** (attempt - 1))
    print(f"\n[oh] transienter Fehler ({label}). "
          f"Versuch {attempt}/{RETRIES}, warte {wait:.0f}s ...")
    time.sleep(wait)
    # Stale-Key-Fehler koennen in der alten Conversation nie heilen -> frische.
    if stale and FRESH_ON_ERROR:
        print("[oh] Schluessel der Conversation ist veraltet -> neue Conversation")
        fresh = new_conversation()
        if fresh:
            cid = fresh
    result, payload = send_and_wait(cid)

print(f"\n(Conversation {cid})")
if result == "answer":
    raise SystemExit(0)
if result == "error":
    retryable, stale, label = payload
    print(f"[oh] FEHLER nach {attempt} Wiederholung(en): {label} "
          f"(retryable={retryable})")
    raise SystemExit(3 if retryable else 4)
print(f"[oh] keine Antwort innerhalb des Zeitfensters ({result})")
raise SystemExit(5)
PY
