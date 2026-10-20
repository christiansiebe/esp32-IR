#!/usr/bin/env python3
"""Sandbox-Server: interaktives Terminal UND Kommando-Kanal.

Drei Rollen auf EINEM Port (der Runtime-Proxy leitet alles an den Port weiter):
  - Pfad "/"            -> interaktives PTY-Terminal (WS -> Shell in der Sandbox)
  - Pfad "/agent"       -> Kommando-Kanal: die Sandbox schickt Befehle, ein Agent
                           auf dem Odroid zeigt sie an und fuehrt sie nach
                           lokaler Bestaetigung aus (Antwort kommt zurueck).
  - Pfad "/ro"          -> READ-ONLY HTTP-GET: ein Aufrufer OHNE Werkzeuge
                           (z.B. ein einfacher Chat/Browser) holt sich die Ausgabe
                           eines erlaubten Lese-Befehls als Klartext:
                             GET /ro?token=<TOKEN>&c=uptime
                           Nur Befehle aus remote-shell/ro_policy.py werden
                           durchgelassen; alles andere wird abgelehnt.

Sicherheit:
  - Alle Pfade nur mit gueltigem Token (hmac.compare_digest).
  - Token aus $SANDBOX_TERM_TOKEN (Datei $SANDBOX_TERM_TOKEN_FILE als Fallback).
  - Ohne Token startet der Server nicht.
  - /ro ist zusaetzlich auf reine Lese-Befehle beschraenkt (ro_policy.check).

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
from websockets.datastructures import Headers
from websockets.http11 import Response

import ro_policy

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
    """Token pro Verbindung frisch lesen. Datei hat Vorrang vor der Umgebung:
    so laesst sich der Token live rotieren (Datei ueberschreiben), ohne dass ein
    veraltetes Cloud-Secret in der Umgebung den Kanal blockiert."""
    path = os.environ.get("SANDBOX_TERM_TOKEN_FILE", "")
    if path and os.path.exists(path):
        tok = open(path).read().strip()
        if tok:
            return tok
    return os.environ.get("SANDBOX_TERM_TOKEN", "").strip()


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
        # Offene Befehle sofort fehlschlagen lassen, sonst blockiert der
        # single-threaded queue_worker bis zum vollen Timeout (Ergebnis ist
        # mit der Verbindung ohnehin verloren).
        for cid, fut in list(PENDING.items()):
            if not fut.done():
                fut.set_result({"code": None, "out": "",
                                "error": "agent disconnected"})
        PENDING.clear()


async def run_remote_command(cmd, timeout=None, ro=False):
    """Befehl an einen Agenten senden und auf das Ergebnis warten.

    ``ro=True`` markiert den Befehl als read-only: der Agent auf dem Odroid
    fuehrt ihn dann OHNE Rueckfrage aus (er prueft die Policy erneut)."""
    if timeout is None:
        timeout = float(os.environ.get("SANDBOX_TERM_CMD_TIMEOUT", "300"))
    if not AGENTS:
        return {"code": None, "out": "", "error": "kein Agent verbunden"}
    cid = next_id()
    fut = asyncio.get_running_loop().create_future()
    PENDING[cid] = fut
    payload = json.dumps({"type": "cmd", "id": cid, "cmd": cmd, "ro": bool(ro)})
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
    ppath = urllib.parse.urlparse(p).path.rstrip("/") or "/"
    peer = getattr(ws, "remote_address", "?")
    if not authorized(p, token):
        audit(f"VERWORFEN (Token) pfad={ppath} von {peer}")
        await ws.close(code=1008, reason="unauthorized")
        return
    audit(f"verbunden pfad={ppath} von {peer}")
    if ppath == "/agent":
        await handle_agent(ws, p)
    else:
        await handle_root(ws)


def _http(status, reason, body, ctype="text/plain; charset=utf-8"):
    headers = Headers()
    headers["Content-Type"] = ctype
    headers["Cache-Control"] = "no-store"
    return Response(status, reason, headers, body if isinstance(body, bytes) else body.encode())


async def _ro_handler(path):
    """READ-ONLY HTTP-Endpunkt /ro - fuer Aufrufer OHNE Werkzeuge.

    Kein WebSocket, nur ein GET, das Klartext zurueckgibt. Nur Befehle, die
    ro_policy.check() durchlaesst, werden ueberhaupt an das Board geschickt;
    der Agent auf dem Odroid prueft die Policy ein zweites Mal.

    Rueckgabe: ein websockets ``Response``-Objekt, damit die WS-Verhandlung
    sofort beendet und ein reiner HTTP-Text geliefert wird.
    """
    q = urllib.parse.parse_qs(urllib.parse.urlparse(path).query)
    token = load_token()
    given = (q.get("token") or [""])[0]
    if not token or not hmac.compare_digest(given, token):
        audit("RO VERWORFEN (Token)")
        return _http(403, "Forbidden", "403 verboten: Token fehlt oder falsch\n")

    cmd = (q.get("c") or [""])[0]
    if not cmd:
        return _http(200, "OK", ro_policy.help_text())

    ok, why = ro_policy.check(cmd)
    if not ok:
        audit(f"RO ABGELEHNT {cmd!r}: {why}")
        return _http(403, "Forbidden", f"403 abgelehnt: {why}\nBefehl: {cmd}\n")

    if not AGENTS:
        audit(f"RO kein Agent fuer {cmd!r}")
        return _http(503, "Service Unavailable",
                     "503 kein Agent verbunden (Odroid-Kanal nicht aktiv)\n")

    # Blockiert den (kurzen) HTTP-Request, bis das Board antwortet. Timeout
    # bewusst klein: der Aufrufer ist ein Chat/Browser, kein Batch-Job.
    timeout = float(os.environ.get("SANDBOX_TERM_RO_TIMEOUT", "30"))
    try:
        res = await run_remote_command(cmd, timeout=timeout, ro=True)
    except Exception as exc:  # noqa: BLE001 - nie den HTTP-Hook crashen lassen
        audit(f"RO FEHLER {cmd!r}: {exc}")
        return _http(500, "Internal Server Error", f"500 interner Fehler: {exc}\n")

    if res.get("error"):
        return _http(504, "Gateway Timeout", f"504 {res['error']}\n")
    out = res.get("out", "")
    code = res.get("code")
    audit(f"RO {cmd!r} code={code} bytes={len(out)}")
    return _http(200, "OK", out if out.endswith("\n") or not out else out + "\n")


async def process_request(connection, request):
    """HTTP-Hook VOR dem WS-Handshake: bedient /ro als reines HTTP-GET.

    Alles andere (WS-Handshake fuer / und /agent) laeuft normal weiter, indem
    None zurueckgegeben wird."""
    path = request.path or ""
    ppath = urllib.parse.urlparse(path).path.rstrip("/") or "/"
    if ppath == "/ro":
        return await _ro_handler(path)
    return None


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
    async with websockets.serve(router, "0.0.0.0", PORT, max_size=None,
                                process_request=process_request):
        print(f"remote-shell lauscht auf 0.0.0.0:{PORT} (Token gesetzt: {len(token)} Zeichen)")
        print(f"Pfade: / (Terminal)  /agent (Kommando-Kanal)  /ro (read-only GET)  CMD_BASE={CMD_BASE}")
        asyncio.create_task(queue_worker())
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
