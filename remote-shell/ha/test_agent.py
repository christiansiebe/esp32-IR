#!/usr/bin/env python3
"""Test: verbindet sich als (Pseudo-)Odroid-Agent ueber die OEFFENTLICHE
Proxy-URL auf den /agent-Pfad und fuehrt Befehle LOKAL aus.

Nur fuer die Verifikation der Sandbox-Seite gedacht:
    python3 remote-shell/ha/test_agent.py [url]
"""
import asyncio
import json
import os
import subprocess
import sys
import urllib.parse

import websockets

_PROJECT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATE = os.environ.get("SANDBOX_TERM_STATE", os.path.join(os.path.dirname(_PROJECT), ".remote-shell"))
TOKEN_FILE = os.environ.get("SANDBOX_TERM_TOKEN_FILE", os.path.join(STATE, "token"))


def default_url():
    rid = os.environ.get("RUNTIME_ID", "")
    return f"wss://work-2-{rid}.prod-runtime.all-hands.dev/"


async def main():
    url = sys.argv[1] if len(sys.argv) > 1 else default_url()
    token = open(TOKEN_FILE).read().strip()
    q = urllib.parse.urlparse(url)
    query = urllib.parse.parse_qs(q.query)
    query["token"] = [token]
    full = q._replace(path="/agent", query=urllib.parse.urlencode(query, doseq=True)).geturl()
    async with websockets.connect(full, max_size=None, open_timeout=15) as ws:
        print("verbunden:", full.split("?")[0])
        await ws.send(json.dumps({"type": "ready"}))
        async for message in ws:
            msg = json.loads(message)
            if msg.get("type") != "cmd":
                continue
            print("BEFEHL:", msg["cmd"])
            p = subprocess.run(msg["cmd"], shell=True, capture_output=True, text=True)
            out = (p.stdout or "") + (p.stderr or "")
            await ws.send(json.dumps(
                {"type": "result", "id": msg["id"], "code": p.returncode, "out": out}))


if __name__ == "__main__":
    asyncio.run(main())
