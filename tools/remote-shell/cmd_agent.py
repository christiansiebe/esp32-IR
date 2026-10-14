#!/usr/bin/env python3
"""Odroid-Agent: empfaengt Befehle aus der Sandbox und fuehrt sie NUR nach
lokaler Bestaetigung aus.

Start (als root auf dem Odroid, in einem eigenen Terminal):
    export SANDBOX_TERM_TOKEN='<token>'
    python3 cmd_agent.py 'wss://work-2-<id>.prod-runtime.all-hands.dev/'

Verhalten:
  - Verbindet AUSGEHEND zur Sandbox (NAT-agnostisch).
  - Bei jedem Befehl: Anzeige + Rueckfrage  ->  [y]=ja  [N]=nein  [a]=immer ja
  - Ctrl-C schaltet die Bestaetigungspflicht AUS/EIN (Pause-Modus).
  - Antwort (Exit-Code + Ausgabe) geht zurueck an die Sandbox.

Voraussetzung:  apt install -y python3-websockets
"""
import asyncio
import json
import os
import signal
import subprocess
import sys
import urllib.parse

import websockets

AUTO = {"always": False, "paused": False}


def tty_prompt(text):
    try:
        with open("/dev/tty", "w") as out, open("/dev/tty", "r") as inp:
            out.write(text)
            out.flush()
            return inp.readline().strip()
    except Exception:
        return ""


async def ask(cmd):
    loop = asyncio.get_running_loop()
    print("\n" + "=" * 60)
    print("BEFEHL AUS DER SANDBOX:")
    print("  " + cmd.replace("\n", "\n  "))
    print("=" * 60)
    if AUTO["always"] and not AUTO["paused"]:
        print("-> automatisch ausgefuehrt (immer-ja aktiv)")
        return True
    ans = await loop.run_in_executor(None, tty_prompt, "Ausfuehren? [y]=ja  [a]=immer ja  [N]=nein : ")
    ans = ans.lower()
    if ans in ("a", "always", "immer"):
        AUTO["always"] = True
        return True
    return ans in ("y", "yes", "j", "ja")


async def run_cmd(cmd, timeout=600):
    try:
        p = await asyncio.create_subprocess_shell(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        try:
            out, _ = await asyncio.wait_for(p.communicate(), timeout=timeout)
        except asyncio.TimeoutError:
            p.kill()
            return {"code": None, "out": "TIMEOUT"}
        return {"code": p.returncode, "out": out.decode(errors="replace")}
    except Exception as exc:  # noqa: BLE001
        return {"code": None, "out": f"AUSFUEHRUNGSFEHLER: {exc}"}


def on_sigint(_sig, _frm):
    AUTO["paused"] = not AUTO["paused"]
    state = "AUS (Bestaetigung aus)" if AUTO["paused"] else "EIN (Bestaetigung noetig)"
    print(f"\n[Ctrl-C] Bestaetigungspflicht jetzt: {state}")


async def session(url, token):
    q = urllib.parse.urlparse(url)
    path = q.path.rstrip("/") + "/agent"
    query = urllib.parse.parse_qs(q.query)
    query["token"] = [token]
    full = q._replace(path=path, query=urllib.parse.urlencode(query, doseq=True)).geturl()
    async with websockets.connect(full, max_size=None, open_timeout=15) as ws:
        print(f"verbunden: {full.split('?')[0]}  (warte auf Befehle ...)")
        await ws.send(json.dumps({"type": "ready"}))
        async for message in ws:
            try:
                msg = json.loads(message)
            except ValueError:
                continue
            if msg.get("type") != "cmd":
                continue
            cid, cmd = msg.get("id"), msg.get("cmd", "")
            if await ask(cmd):
                res = await run_cmd(cmd)
            else:
                print("-> ABGELEHNT")
                res = {"code": -1, "out": "ABGELEHNT (keine Bestaetigung)"}
            res.update({"type": "result", "id": cid})
            await ws.send(json.dumps(res))


async def main():
    if len(sys.argv) < 2:
        raise SystemExit("Aufruf: cmd_agent.py 'wss://work-<n>-<id>.prod-runtime.all-hands.dev/'")
    token = os.environ.get("SANDBOX_TERM_TOKEN", "")
    if not token:
        raise SystemExit("SANDBOX_TERM_TOKEN nicht gesetzt.")
    signal.signal(signal.SIGINT, on_sigint)
    url = sys.argv[1]
    while True:
        try:
            await session(url, token)
        except Exception as exc:  # noqa: BLE001
            print(f"[{exc}] - neuer Versuch in 5s ...")
            await asyncio.sleep(5)


if __name__ == "__main__":
    asyncio.run(main())
