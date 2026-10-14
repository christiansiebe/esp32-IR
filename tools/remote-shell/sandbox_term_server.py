#!/usr/bin/env python3
"""Sandbox-Server: interaktives Terminal UND Kommando-Kanal.

Zwei Rollen auf EINEM Port (der Runtime-Proxy leitet alles an den Port weiter):
  - Pfad "/"            -> interaktives PTY-Terminal (WS -> Shell in der Sandbox)
  - Pfad "/agent"       -> Kommando-Kanal: die Sandbox schickt Befehle, ein Agent
                           auf dem Odroid zeigt sie an und fuehrt sie nach
                           lokaler Bestaetigung aus (Antwort kommt zurueck).

Sicherheit:
  - Beide Pfade nur mit gueltigem Token (hmac.compare_digest).
  - Token aus $SANDBOX_TERM_TOKEN (Datei $SANDBOX_TERM_TOKEN_FILE als Fallback).
  - Ohne Token startet der Server nicht.

Kommando-Kanal, Dateien unter $CMD_BASE (Default /tmp/cmdchannel):
  - queue/<id>.cmd     -> hier legt die Sandbox Befehle ab (siehe cmd_send.sh)
  - results/<id>.out   -> Ausgabe des Befehls
  - results/<id>.code  -> Exit-Code
  - results/<id>.json  -> vollstaendiges Ergebnis
  - audit.log          -> jede Ausfuehrung/ablehnung protokolliert

Protokoll (Agent, Text-Frames = JSON):
  Agent -> Server: {"type":"ready"}
  Server -> Agent: {"type":"cmd","id":...,"cmd":"..."}
  Agent -> Server: {"type":"result","id":...,"code":...,"out":"..."}
Terminal-Protokoll: Binaer = rohe Bytes, Text = {"type":"resize",...}.
"""
import asyncio
import fcntl
import hmac
import json
import os
import pty
import signal
import struct
import termios
import time
import urllib.parse

import websockets

PORT = int(os.environ.get("SANDBOX_TERM_PORT", "12001"))
SHELL = os.environ.get("SANDBOX_TERM_SHELL", "/bin/bash")
CMD_BASE = os.environ.get("CMD_BASE", "/tmp/cmdchannel")
CMD_QUEUE = os.path.join(CMD_BASE, "queue")
CMD_RESULTS = os.path.join(CMD_BASE, "results")
CMD_LIVE = os.path.join(CMD_BASE, "live")

AGENTS = set()          # aktive Kommando-Kanal-Verbindungen
PENDING = {}            # id -> Future
COUNTER = {"n": 0}


def audit(line):
    os.makedirs(CMD_BASE, exist_ok=True)
    with open(os.path.join(CMD_BASE, "audit.log"), "a") as fh:
        fh.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {line}\n")


def next_id():
    COUNTER["n"] += 1
    return f"{int(time.time())}-{COUNTER['n']:04d}"


def load_token():
    tok = os.environ.get("SANDBOX_TERM_TOKEN", "")
    if not tok:
        path = os.environ.get("SANDBOX_TERM_TOKEN_FILE", "")
        if path and os.path.exists(path):
            tok = open(path).read().strip()
    return tok


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def authorized(path, token):
    if not token:
        return False
    params = urllib.parse.parse_qs(urllib.parse.urlparse(path).query)
    given = (params.get("token") or [""])[0]
    return hmac.compare_digest(given, token)


async def pump_pty_to_ws(ws, master):
    loop = asyncio.get_running_loop()
    queue = asyncio.Queue()

    def on_readable():
        try:
            data = os.read(master, 65536)
        except OSError:
            data = b""
        queue.put_nowait(data)

    loop.add_reader(master, on_readable)
    try:
        while True:
            data = await queue.get()
            if not data:
                break
            await ws.send(data)
    finally:
        loop.remove_reader(master)


async def handle_agent(ws, path):
    """Kommando-Kanal: Agent auf dem Odroid verbindet sich hier."""
    AGENTS.add(ws)
    audit(f"agent connected ({len(AGENTS)} aktiv)")
    try:
        async for message in ws:
            try:
                msg = json.loads(message)
            except ValueError:
                continue
            if msg.get("type") == "ready":
                continue
            if msg.get("type") == "progress":
                cid = msg.get("id")
                os.makedirs(CMD_LIVE, exist_ok=True)
                with open(os.path.join(CMD_LIVE, str(cid) + ".log"), "a") as fh:
                    fh.write(msg.get("chunk", ""))
                continue
            if msg.get("type") == "result":
                cid = msg.get("id")
                fut = PENDING.pop(cid, None)
                if fut and not fut.done():
                    fut.set_result(msg)
    except websockets.ConnectionClosed:
        pass
    finally:
        AGENTS.discard(ws)
        audit("agent disconnected")


async def run_remote_command(cmd, timeout=120):
    """Befehl an einen Agenten senden und auf das Ergebnis warten."""
    if not AGENTS:
        return {"code": None, "out": "", "error": "kein Agent verbunden"}
    cid = next_id()
    fut = asyncio.get_running_loop().create_future()
    PENDING[cid] = fut
    payload = json.dumps({"type": "cmd", "id": cid, "cmd": cmd})
    for ws in list(AGENTS):
        try:
            await ws.send(payload)
        except Exception:
            AGENTS.discard(ws)
    try:
        res = await asyncio.wait_for(fut, timeout=timeout)
    except asyncio.TimeoutError:
        PENDING.pop(cid, None)
        audit(f"TIMEOUT {cid}")
        return {"code": None, "out": "", "error": "timeout"}
    os.makedirs(CMD_RESULTS, exist_ok=True)
    with open(os.path.join(CMD_RESULTS, cid + ".out"), "w") as fh:
        fh.write(res.get("out", ""))
    with open(os.path.join(CMD_RESULTS, cid + ".json"), "w") as fh:
        json.dump(res, fh)
    audit(f"result {cid} code={res.get('code')}")
    return res


def _queue_path(cid):
    os.makedirs(CMD_QUEUE, exist_ok=True)
    return os.path.join(CMD_QUEUE, cid + ".cmd")


async def handle_root(ws):
    """Interaktives PTY-Terminal (Pfad /)."""
    token = load_token()
    path = ws.request.path if getattr(ws, "request", None) is not None else ""
    if not authorized(path, token):
        await ws.close(code=1008, reason="unauthorized")
        return

    master, slave = pty.openpty()
    set_winsize(master, 24, 80)
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.close(master)
        os.close(slave)
        os.execvp(SHELL, [SHELL, "-i"])
    os.close(slave)

    reader = asyncio.create_task(pump_pty_to_ws(ws, master))
    try:
        async for message in ws:
            if isinstance(message, (bytes, bytearray)):
                os.write(master, bytes(message))
            else:
                try:
                    ctl = json.loads(message)
                except ValueError:
                    continue
                if ctl.get("type") == "resize":
                    set_winsize(master, int(ctl.get("rows", 24)), int(ctl.get("cols", 80)))
    except websockets.ConnectionClosed:
        pass
    finally:
        reader.cancel()
        try:
            os.kill(pid, signal.SIGHUP)
        except ProcessLookupError:
            pass
        os.close(master)


async def router(ws, path=None):
    """Weiche: /agent -> Kommando-Kanal, sonst -> PTY-Terminal."""
    token = load_token()
    p = path if path is not None else (
        ws.request.path if getattr(ws, "request", None) is not None else ""
    )
    if not authorized(p, token):
        await ws.close(code=1008, reason="unauthorized")
        return
    if urllib.parse.urlparse(p).path.rstrip("/") == "/agent":
        await handle_agent(ws, p)
    else:
        await handle_root(ws)


async def queue_worker():
    """Befehle aus queue/*.cmd ausfuehren (Datei-Kanal, siehe cmd_send.sh)."""
    while True:
        os.makedirs(CMD_QUEUE, exist_ok=True)
        for name in sorted(os.listdir(CMD_QUEUE)):
            if not name.endswith(".cmd"):
                continue
            cid = name[:-4]
            full = os.path.join(CMD_QUEUE, name)
            try:
                cmd = open(full).read()
            except OSError:
                continue
            os.rename(full, full + ".running")
            res = await run_remote_command(cmd)
            os.makedirs(CMD_RESULTS, exist_ok=True)
            with open(os.path.join(CMD_RESULTS, cid + ".json"), "w") as fh:
                json.dump(res, fh)
            with open(os.path.join(CMD_RESULTS, cid + ".out"), "w") as fh:
                fh.write(res.get("out", ""))
            with open(os.path.join(CMD_RESULTS, cid + ".code"), "w") as fh:
                fh.write(str(res.get("code")))
            os.remove(full + ".running")
        await asyncio.sleep(1)


async def main():
    token = load_token()
    if not token:
        raise SystemExit("SANDBOX_TERM_TOKEN nicht gesetzt - Start abgebrochen.")
    async with websockets.serve(router, "0.0.0.0", PORT, max_size=None):
        print(f"remote-shell lauscht auf 0.0.0.0:{PORT} (Token gesetzt: {len(token)} Zeichen)")
        print(f"Pfade: / (Terminal)  /agent (Kommando-Kanal)  CMD_BASE={CMD_BASE}")
        asyncio.create_task(queue_worker())
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
