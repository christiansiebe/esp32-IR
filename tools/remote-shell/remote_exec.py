#!/usr/bin/env python3
"""remote_exec.py — Befehl in einer ANDEREN Sandbox ausfuehren (Cloud-API).

Nutzt die OpenHands-Cloud-API, um Session-Key + Agent-Server-URL einer
Conversation zu holen, und ruft dort den Agent-Server-Bash-Endpunkt auf.
So kann diese Sandbox eine andere Sandbox automatisch einrichten (Bootstrap),
ohne dass der Nutzer dort etwas eingeben muss.

Aufruf:  python3 remote_exec.py <conversation_id> "<befehl>"
"""
import json
import os
import sys
import urllib.error
import urllib.request

KEY = os.environ.get("OPENHANDS_API_KEY", "")
BASE = "https://app.all-hands.dev"


def get(path):
    req = urllib.request.Request(BASE + path)
    req.add_header("Authorization", f"Bearer {KEY}")
    req.add_header("Accept", "application/json")
    with urllib.request.urlopen(req, timeout=25) as r:
        return json.loads(r.read())


def conv(cid):
    d = get(f"/api/v1/app-conversations?ids={cid}")
    items = d.get("items", d) if isinstance(d, dict) else d
    return items[0] if isinstance(items, list) and items else items


def exec_bash(agent_url, session_key, command, timeout=120):
    body = json.dumps({"command": command, "timeout": timeout}).encode()
    req = urllib.request.Request(agent_url + "/api/bash/execute_bash_command", data=body, method="POST")
    req.add_header("X-Session-API-Key", session_key)
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout + 30) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return {"error": e.code, "detail": e.read().decode()[:400]}


def main():
    cid, command = sys.argv[1], sys.argv[2]
    c = conv(cid)
    agent_url = (c.get("conversation_url") or "").rsplit("/api/conversations", 1)[0]
    sk = c.get("session_api_key")
    print(f"# conversation={cid} sandbox={c.get('sandbox_id')} status={c.get('sandbox_status')}")
    if not (agent_url and sk):
        print("kein session_api_key/conversation_url verfuegbar"); return 1
    out = exec_bash(agent_url, sk, command)
    print(json.dumps(out, indent=1)[:3000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
