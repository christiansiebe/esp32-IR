#!/usr/bin/env python3
"""Buddy-Watchdog: zwei OpenHands-Conversations in EINER Sandbox passen
gegenseitig auf sich auf, damit keine durch einen abgebrochenen Lauf haengen
bleibt (execution_status=error) und die Sandbox nicht idle einschlaeft.

- Jede Conversation hat einen Heartbeat (Datei + Zeitstempel).
- Der Watchdog prueft die Partner-Conversation: ist sie zu lange still ODER in
  error/finished, wird sie per lokalem agent-server 'geweckt' (kurze Nachricht).
- Der eigene Beat ist zugleich Conversation-Aktivitaet -> haelt die Sandbox wach.

Kein Cloud-API-Key noetig: alles ueber http://127.0.0.1:<PORT> mit
Header X-Session-API-Key.

  python3 buddy.py register <name> [conv_id]
  python3 buddy.py beat     <name>
  python3 buddy.py watch    <name> [--loop]
  python3 buddy.py status
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

STATE_DIR = os.environ.get("BUDDY_STATE_DIR", "/workspace/.oh-buddy")
PORT = os.environ.get("BUDDY_AGENT_PORT", os.environ.get("AGENT_PORT", "60000"))
BASE = "http://127.0.0.1:" + PORT
SESSION = os.environ.get("SESSION_API_KEY", "")
STALE = float(os.environ.get("BUDDY_STALE", "600"))
POLL = float(os.environ.get("BUDDY_POLL", "60"))
# Nur wecken, wenn WIRKLICH Arbeit offen ist (sonst Token-Kosten!):
WAKE_COOLDOWN = float(os.environ.get("BUDDY_WAKE_COOLDOWN", "900"))   # 15 min
MAX_WAKES = int(os.environ.get("BUDDY_MAX_WAKES", "2"))
WAKE_MSG = ("System (Buddy-Watchdog): Dein Lauf scheint unterbrochen. Bitte mache "
            "mit deiner offenen Aufgabe weiter. Wenn nichts offen ist, antworte "
            "kurz 'OK, nichts offen'. Automatische Nachricht.")


def _req(method, path, body=None, timeout=25):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Content-Type": "application/json",
                                          "X-Session-API-Key": SESSION})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")[:300]
    except Exception as e:  # noqa: BLE001
        return 0, str(e)[:200]


def _path(name):
    os.makedirs(STATE_DIR, exist_ok=True)
    return os.path.join(STATE_DIR, name + ".json")


def _load(name):
    try:
        return json.load(open(_path(name)))
    except Exception:  # noqa: BLE001
        return {}


def _save(name, data):
    tmp = _path(name) + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(data, fh)
    os.replace(tmp, _path(name))


def register(name, conv_id=None):
    st = _load(name)
    st.update(name=name, conv_id=conv_id or st.get("conv_id"),
              last_beat=time.time(), registered_at=st.get("registered_at", time.time()))
    _save(name, st)
    print("registriert:", name, "conv=", st["conv_id"], "->", _path(name))


def beat(name):
    st = _load(name)
    st.update(name=name, last_beat=time.time())
    _save(name, st)
    print("beat", name, time.strftime("%H:%M:%S"))


def all_buddies():
    if not os.path.isdir(STATE_DIR):
        return []
    return [_load(fn[:-5]) for fn in os.listdir(STATE_DIR) if fn.endswith(".json")]


def wake(conv_id, msg=WAKE_MSG):
    return _req("POST", "/api/conversations/" + conv_id + "/events",
                {"role": "user", "content": [{"type": "text", "text": msg}], "run": True})


def conv_status(conv_id):
    st, o = _req("GET", "/api/conversations/" + conv_id)
    if st != 200:
        return st, None
    try:
        return st, json.loads(o).get("execution_status")
    except Exception:  # noqa: BLE001
        return st, None


def _events(cid, limit=20):
    st, o = _req("GET", "/api/conversations/%s/events/search?limit=%d&sort_order=TIMESTAMP_DESC" % (cid, limit))
    if st != 200:
        return None
    try:
        return json.loads(o).get("items", [])
    except Exception:  # noqa: BLE001
        return None


def needs_wake(cid):
    """True nur, wenn eine User-Nachricht ohne Antwort/Finish dasteht."""
    items = _events(cid)
    if items is None:
        return False
    # Neuestes entscheidendes Event zuerst betrachten (DESC-Reihenfolge).
    for e in items:
        k = e.get("kind")
        if k == "MessageEvent":
            role = (e.get("llm_message") or {}).get("role")
            if role == "user":
                return True          # letzte Aktion war die User-Nachricht -> offen
            if role == "assistant":
                return False         # letzte Aktion war die Antwort -> fertig
        elif k == "ActionEvent":
            if e.get("tool_name") == "finish":
                return False         # sauber beendet
            return True              # mitten in einer Aktion abgebrochen
        # alles andere (State/Observation/Error) ueberspringen
    return False


def _fleet_open_tasks():
    out = {}
    try:
        d = json.load(open('/workspace/fleet/agents.json'))
    except Exception:
        return out
    for k, v in (d.get('agents') or {}).items():
        try:
            out[k] = int(v.get('open_tasks', 0))
        except Exception:
            out[k] = 0
    return out

def watch(name, loop=False):
    while True:
        now = time.time()
        partners = [b for b in all_buddies() if b.get("name") != name and b.get("conv_id")]
        if not partners:
            print("kein Partner registriert - warte")
        for b in partners:
            age = now - float(b.get("last_beat") or 0)
            cid = b["conv_id"]
            cst, status = conv_status(cid)
            print("partner %s (%s) beat_age=%.0fs status=%s http=%s"
                  % (b["name"], cid[:8], age, status, cst))
            if age > STALE and status in ("error", "finished", "idle", None):
                wst, wo = wake(cid)
                print("  -> wake %s http=%s %s" % (cid[:8], wst, wo[:80]))
                b["last_beat"] = now
                _save(b["name"], b)
        beat(name)
        if not loop:
            return
        time.sleep(POLL)


def watchall(loop=False):
    """Deterministisch: alle registrierten Conversations ueberwachen und eine
    stehengebliebene (error/finished/stale) per lokalem API wecken. Laeuft im
    Supervisor, damit die Ueberwachung einen Sandbox-Reset ueberlebt."""
    while True:
        now = time.time()
        open_tasks = _fleet_open_tasks()
        for b in all_buddies():
            cid = b.get("conv_id")
            if not cid:
                continue
            cst, status = conv_status(cid)
            if cst != 200 or status != "error":
                continue
            if int(b.get("wakes", 0)) >= MAX_WAKES:
                continue
            if now - float(b.get("last_wake") or 0) < WAKE_COOLDOWN:
                continue
            if not needs_wake(cid):
                print("skip %s (%s, error, nichts offen)" % (b.get("name"), cid[:8]))
                continue
            if open_tasks.get(cid.replace("-", ""), 0) <= 0:
                print("skip %s (%s, kein open_tasks>0)" % (b.get("name"), cid[:8]))
                continue
            wst, wo = wake(cid)
            b["wakes"] = int(b.get("wakes", 0)) + 1
            b["last_wake"] = now
            _save(b["name"], b)
            print("wake %s (%s, error, %d/%d) http=%s %s"
                  % (b.get("name"), cid[:8], b["wakes"], MAX_WAKES, wst, wo[:50]))
        for b in all_buddies():
            b["last_beat"] = now
            _save(b["name"], b)
        if not loop:
            return
        time.sleep(POLL)


def status():
    for b in all_buddies():
        age = time.time() - float(b.get("last_beat") or 0)
        print("%-12s conv=%-8s beat_age=%6.0fs" % (b.get("name"), str(b.get("conv_id"))[:8], age))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]
    if cmd == "register":
        register(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    elif cmd == "beat":
        beat(sys.argv[2])
    elif cmd == "watchall":
        watchall("--loop" in sys.argv)
    elif cmd == "watch":
        watch(sys.argv[2], "--loop" in sys.argv)
    elif cmd == "status":
        status()
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
