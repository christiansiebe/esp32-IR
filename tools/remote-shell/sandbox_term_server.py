#!/usr/bin/env python3
"""Token-authentifizierter WebSocket -> PTY-Server (Sandbox-Seite).

Stellt eine interaktive Shell in der Sandbox bereit, erreichbar ueber den
Runtime-Proxy (wss://work-<n>-...prod-runtime.all-hands.dev/). Es lauscht auf
0.0.0.0:<PORT>; nach aussen sichtbar wird es nur ueber den Proxy.

Sicherheit:
  - Zugriff nur mit gueltigem Token (konstanter Vergleich, hmac.compare_digest).
  - Token kommt aus $SANDBOX_TERM_TOKEN (Datei $SANDBOX_TERM_TOKEN_FILE als Fallback).
  - Ohne gesetztes Token startet der Server NICHT.
  - Bindet an 0.0.0.0 nur, weil der Proxy lokal weiterleitet.

Protokoll:
  - Binaer-Frame  = rohe Terminal-Bytes (stdin/stdout)
  - Text-Frame    = JSON-Steuerung, z. B. {"type":"resize","cols":80,"rows":24}
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
import urllib.parse

import websockets

PORT = int(os.environ.get("SANDBOX_TERM_PORT", "12001"))
SHELL = os.environ.get("SANDBOX_TERM_SHELL", "/bin/bash")


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


async def handle(ws):
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


async def main():
    token = load_token()
    if not token:
        raise SystemExit("SANDBOX_TERM_TOKEN nicht gesetzt - Start abgebrochen.")
    async with websockets.serve(handle, "0.0.0.0", PORT, max_size=None):
        print(f"remote-shell lauscht auf 0.0.0.0:{PORT} (Token gesetzt: {len(token)} Zeichen)")
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
