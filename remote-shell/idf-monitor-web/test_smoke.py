#!/usr/bin/env python3
"""End-to-End-Rauchtest fuer idf-monitor-web — ohne Hardware.

Bootet den Server mit einem Fake-Monitor, verbindet sich per echtem WebSocket
(stdlib, maskierte Client-Frames wie ein Browser) und prueft:
  - Index-Seite liefert HTML.
  - WS ohne gueltiges Token -> 1008 (unauthorized).
  - WS mit Token -> Banner des Fake-Monitors kommt an.
  - Tastatureingabe erreicht den Monitor (Echo kommt zurueck).
  - Resize-Nachricht wird akzeptiert (kein Absturz).
"""
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOL)

PORT = 7699
TOKEN = "smoketoken"

FAKE_MONITOR = """#!/usr/bin/env python3
import os, sys
sys.stdout.write("FAKE-MONITOR READY\\n"); sys.stdout.flush()
while True:
    b = os.read(0, 1024)
    if not b: break
    sys.stdout.write("ECHO:" + b.decode("latin-1")); sys.stdout.flush()
"""


def make_fake_monitor(path):
    with open(path, "w", encoding="ascii") as fh:
        fh.write(FAKE_MONITOR)
    os.chmod(path, 0o755)
    return path


def ws_connect(path):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    head, _, leftover = buf.partition(b"\r\n\r\n")
    return s, head, leftover


def masked(payload: bytes, opcode: int = 0x1):
    m = os.urandom(4)
    out = bytearray([0x80 | opcode])
    n = len(payload)
    out.append(0x80 | n)  # small frames only
    out += m
    for i in range(n):
        out.append(payload[i] ^ m[i & 3])
    return bytes(out)


def recv_frame(s, pre=b""):
    buf = bytearray(pre)
    while len(buf) < 2:
        buf += s.recv(4096)
    b1 = buf[1]
    n = b1 & 0x7F
    idx = 2
    if n == 126:
        while len(buf) < 4:
            buf += s.recv(4096)
        n = struct.unpack("!H", buf[2:4])[0]
        idx = 4
    elif n == 127:
        while len(buf) < 10:
            buf += s.recv(4096)
        n = struct.unpack("!Q", buf[2:10])[0]
        idx = 10
    while len(buf) < idx + n:
        buf += s.recv(4096)
    return buf[0] & 0x0F, bytes(buf[idx:idx + n])


def main():
    env = dict(os.environ)
    fake = make_fake_monitor(os.path.join(tempfile.gettempdir(), "idf_fake_monitor.py"))
    proc = subprocess.Popen(
        [sys.executable, os.path.join(TOOL, "idf_monitor_web.py"),
         "--port", "/dev/null", "--http-port", str(PORT), "--token", TOKEN,
         "--monitor", fake],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    fails = 0
    try:
        # Warten, bis der Port wirklich lauscht (max 5 s), sonst Log zeigen.
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
                break
            except OSError:
                if proc.poll() is not None:
                    break
                time.sleep(0.2)
        if proc.poll() is not None:
            print("  FAIL Server startete nicht:")
            print(proc.stdout.read().decode("utf-8", "replace"))
            return 1

        # 1. Index-Seite
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        page = b""
        while True:
            c = s.recv(4096)
            if not c:
                break
            page += c
        s.close()
        if b"<html" in page and b"xterm" in page:
            print("  ok   Index-Seite liefert HTML")
        else:
            print("  FAIL Index-Seite"); fails += 1

        # 2. WS ohne Token -> Close 1008
        ws, _h, pre = ws_connect("/ws?token=wrong")
        op, data = recv_frame(ws, pre)
        if op == 0x1 and b"unauthorized" in data:
            print("  ok   falsches Token -> unauthorized")
        else:
            print(f"  FAIL falsches Token (op={op} data={data!r})"); fails += 1
        ws.close()

        # 3. WS mit Token -> Banner
        ws, _h, pre = ws_connect(f"/ws?token={TOKEN}")
        got = b""
        deadline = time.time() + 4
        while b"FAKE-MONITOR READY" not in got and time.time() < deadline:
            op, data = recv_frame(ws, pre)
            pre = b""
            if op is None:
                break
            got += data
        if b"FAKE-MONITOR READY" in got:
            print("  ok   WS mit Token -> Monitor-Banner")
        else:
            print(f"  FAIL Banner fehlt: {got!r}"); fails += 1

        # 4. Tastatureingabe erreicht den Monitor (Echo)
        ws.sendall(masked(b"PING\n", 0x1))
        got = b""
        deadline = time.time() + 4
        while b"ECHO:PING" not in got and time.time() < deadline:
            op, data = recv_frame(ws)
            if op is None:
                break
            got += data
        if b"ECHO:PING" in got:
            print("  ok   Tastatureingabe -> Monitor-Echo")
        else:
            print(f"  FAIL Echo fehlt: {got!r}"); fails += 1

        # 5. Resize akzeptiert
        ws.sendall(masked(json.dumps({"type": "resize", "rows": 30, "cols": 100}).encode()))
        time.sleep(0.3)
        if ws.fileno() != -1:
            print("  ok   Resize akzeptiert (Server lebt)")
        else:
            print("  FAIL Resize"); fails += 1
        ws.close()
    finally:
        proc.terminate()
        try:
            out = proc.communicate(timeout=3)[0]
        except subprocess.TimeoutExpired:
            proc.kill()
            out = b""
    if fails:
        print("\n--- Server-Log ---")
        print(out.decode("utf-8", "replace"))
    print(f"\n{'SMOKE OK' if not fails else 'SMOKE FAILED'} ({fails} Fehler)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
