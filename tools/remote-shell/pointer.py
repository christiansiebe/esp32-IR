#!/usr/bin/env python3
"""pointer.py — Cloud-API-Zeiger auf das geteilte Gedaechtnis setzen.

Setzt fuer alle (oder ausgewaehlte) OpenHands-Conversations:
  - Titel/Tag mit dem Zeiger auf memory/SHARED.md
  - eine Chat-Nachricht (run=false, weckt den Agenten NICHT) mit der Anleitung

So findet JEDER Chat sofort ohne Nutzereingabe das gemeinsame Gedaechtnis
und die Odroid-Anleitung.

Aufruf:
  python3 pointer.py                 # alle Conversations
  python3 pointer.py <id> [<id> ...] # nur diese
"""
import json
import os
import sys
import urllib.error
import urllib.request

KEY = os.environ.get("OPENHANDS_API_KEY", "")
BASE = "https://app.all-hands.dev"
SHARED = "https://github.com/christiansiebe/esp32-IR/blob/main/memory/SHARED.md"
TAG = "shared-memory:memory/SHARED.md"

MESSAGE = (
    "[Automatischer Zeiger — gemeinsames Gedaechtnis]\n\n"
    "Dieses Projekt hat EIN gemeinsames, persistentes Gedaechtnis, das alle "
    "OpenHands-Chats teilen:\n"
    f"- GitHub: {SHARED}\n"
    "- Live in dieser Sandbox: /workspace/MEMORY.md\n"
    "  (Symlink: ~/.openhands/memory/MEMORY.md)\n\n"
    "Bitte vor der Arbeit lesen und am Ende ergaenzen. "
    "Odroid-Kanal (falls eingerichtet): bash /workspace/remote-shell/cmd_send.sh \"<befehl>\"\n"
    "Anleitung: tools/remote-shell/OTHER-CHAT.md"
)


def api(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method)
    req.add_header("Authorization", f"Bearer {KEY}")
    req.add_header("Accept", "application/json")
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except Exception:
            return e.code, {}


def all_ids():
    st, d = api("GET", "/api/v1/app-conversations/search?limit=100")
    items = d.get("items", []) if st == 200 else []
    return [c["id"] for c in items]


def mark(cid, with_message=True):
    st1, _ = api("PATCH", f"/api/v1/app-conversations/{cid}", {"tags": {"memory": TAG}})
    st2 = "-"
    if with_message:
        st2, _ = api("POST", f"/api/v1/app-conversations/{cid}/send-message",
                     {"role": "user", "content": [{"type": "text", "text": MESSAGE}], "run": False})
    return st1, st2


def main():
    if not KEY:
        print("OPENHANDS_API_KEY fehlt."); return 2
    args = sys.argv[1:]
    with_message = "--tag-only" not in args
    ids = [a for a in args if not a.startswith("--")] or all_ids()
    print(f"{len(ids)} Conversation(s), Nachricht={'nein' if not with_message else 'ja'}")
    for cid in ids:
        st1, st2 = mark(cid, with_message)
        print(f"  {cid}: tag={st1} msg={st2}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
