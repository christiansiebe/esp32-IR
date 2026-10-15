#!/usr/bin/env python3
"""Token-authentifizierter PTY -> WebSocket-Client (Odroid-Seite).

Verbindet sich AUSGEHEND vom Odroid zur Sandbox (NAT-agnostisch) und gibt die
Sandbox-Shell im lokalen Terminal aus. Aufruf auf dem Odroid:

    export SANDBOX_TERM_TOKEN='<dein-token>'
    python3 sandbox_term_client.py 'wss://work-2-<id>.prod-runtime.all-hands.dev/'

Voraussetzungen auf dem Odroid:  python3 -m pip install websockets
Beenden:  Ctrl-]   (oder den Prozess killen)
"""
import asyncio
import os
import select
import signal
import struct
import sys
import termios
import tty
import urllib.parse

import websockets

STDIN = 0
STDOUT = 1


def get_winsize():
    try:
        rows, cols = os.popen("stty size", "r").read().split()
        return int(rows), int(cols)
    except Exception:
        return 24, 80


def resize_payload():
    rows, cols = get_winsize()
    return '{"type":"resize","rows":%d,"cols":%d}' % (rows, cols)


async def run(url, token):
    q = urllib.parse.urlparse(url)
    query = urllib.parse.parse_qs(q.query)
    query["token"] = [token]
    full = q._replace(query=urllib.parse.urlencode(query, doseq=True)).geturl()

    async with websockets.connect(full, max_size=None) as ws:
        await ws.send(resize_payload())
        old = termios.tcgetattr(STDIN)
        tty.setraw(STDIN)
        loop = asyncio.get_running_loop()
        stop = asyncio.Event()

        def on_stdin():
            data = os.read(STDIN, 4096)
            if not data:
                stop.set()
                return
            asyncio.ensure_future(ws.send(data))

        def on_resize(_sig, _frm):
            asyncio.ensure_future(ws.send(resize_payload()))

        loop.add_reader(STDIN, on_stdin)
        signal.signal(signal.SIGWINCH, on_resize)

        async def from_ws():
            try:
                async for message in ws:
                    if isinstance(message, (bytes, bytearray)):
                        os.write(STDOUT, message)
                    else:
                        print(message)
            except websockets.ConnectionClosed:
                stop.set()

        reader = asyncio.create_task(from_ws())
        await stop.wait()
        reader.cancel()
        loop.remove_reader(STDIN)
        termios.tcsetattr(STDIN, termios.TCSADRAIN, old)
        print("\n[Verbindung beendet]")


def main():
    if len(sys.argv) < 2:
        raise SystemExit("Aufruf: sandbox_term_client.py 'wss://work-<n>-<id>.prod-runtime.all-hands.dev/'")
    token = os.environ.get("SANDBOX_TERM_TOKEN", "")
    if not token:
        raise SystemExit("SANDBOX_TERM_TOKEN nicht gesetzt.")
    asyncio.run(run(sys.argv[1], token))


if __name__ == "__main__":
    main()
