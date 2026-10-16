#!/usr/bin/env python3
"""idf_monitor_web — den seriellen IDF-Monitor im Browser bedienbar machen.

Laeuft auf dem HOST (Odroid), NICHT auf dem ESP32. Der ESP32 weiss nicht, wer
seine UART0-Bytes liest; die Firmware bleibt unveraendert. Der serielle Adapter
haengt am Odroid (`/dev/ttyUSB*` bzw. `/dev/ttyACM*`).

Architektur:
    Browser (xterm.js) <-> WebSocket <-> dieser Server <-> PTY <-> esp-idf-monitor <-> /dev/ttyUSB0

Warum ein eigenes PTY: `esp-idf-monitor` braucht `isatty()` und Raw-Mode auf
seinen eigenen stdio und setzt DTR/RTS per `ioctl` (Chip-Reset). Laeuft es mit
einer Pipe als stdin, scheitert es mit `[Errno 25] Inappropriate ioctl for
device`. Mit dem PTY funktionieren alle Monitor-Tasten nativ.

Mehrere Browser-Tabs teilen sich genau EINEN Monitor-Prozess (Fan-out): zwei
Lese-Prozesse auf demselben Port erzeugen sonst Datenmuell.

Start:
    ./idf_monitor_web.py --port auto --baud 115200 --http-port 7681
    # Browser: http://<odroid>:7681/?token=<TOKEN>
"""

import argparse
import fcntl
import hmac
import json
import os
import pty
import queue
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wsproto  # noqa: E402

DEFAULT_BAUD = 115200
DEFAULT_COLS, DEFAULT_ROWS = 120, 40
CFG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "state")


# --------------------------------------------------------------------------
# Port-Erkennung: NUR echte USB-Serial-Adapter. Niemals die Odroid-Konsole.
# --------------------------------------------------------------------------
FORBIDDEN = re.compile(r"ttyAML|ttyS\d|ttyFIQ|console", re.I)


def detect_ports():
    """Liefert die Kandidaten fuer USB-Serial-Adapter, stabil sortiert.

    Bevorzugt `/dev/serial/by-id/*` (stabil ueber Reboots), dann die rohen
    Geraete. `ttyAML*` (Odroid-Konsole) und `ttyS*` sind ausgeschlossen — sonst
    haengt man an der Board-Konsole statt am ESP32.
    """
    candidates = []
    by_id = "/dev/serial/by-id"
    if os.path.isdir(by_id):
        for name in sorted(os.listdir(by_id)):
            candidate = os.path.realpath(os.path.join(by_id, name))
            if not FORBIDDEN.search(os.path.basename(candidate)):
                candidates.append(candidate)
    if not candidates:
        for base in ("/dev/ttyUSB", "/dev/ttyACM"):
            for i in range(8):
                dev = f"{base}{i}"
                if os.path.exists(dev):
                    candidates.append(dev)
    seen, out = set(), []
    for c in candidates:
        if c not in seen:
            seen.add(c)
            out.append(c)
    return out


def resolve_port(spec):
    """`auto` -> erster USB-Adapter; sonst der angegebene Pfad (mit Pruefung)."""
    if spec in (None, "", "auto"):
        ports = detect_ports()
        if not ports:
            raise SystemExit("Kein USB-Serial-Adapter gefunden. Steckt der ESP32? "
                             "Alternativ --port /dev/ttyUSB0 angeben.")
        return ports[0]
    if FORBIDDEN.search(os.path.basename(spec)):
        raise SystemExit(f"{spec} sieht nach Board-Konsole aus, nicht nach ESP32 — "
                         "abgelehnt.")
    return spec


# --------------------------------------------------------------------------
# Tastenkonflikt: Browser frisst Ctrl+T/R/P/F/L/I/Y/H. Umlagen per Monitor-CFG.
# --------------------------------------------------------------------------
# esp-idf-monitor liest die Tasten aus einer ini ([esp-idf-monitor]). Nur Keys,
# die als C0-Codes darstellbar sind, taugen: A-Z und [ ] \ ^ _ .
# Browser fangen Ctrl+T/R/P/F/L/I/Y/H (und D, N, S, U, W, O, K, E, B, C, V) ab.
# Deshalb bleiben nur wenige Buchstaben (G, M, Q, X, Z) plus die Symboltasten
# uebrig. Die Monitor-Funktionen werden darauf umgelegt; `exit_key` bleibt `]`,
# damit der gewohnte Ausstieg funktioniert.
BROWSER_BOUND = set("TRPFLIYHDNSUWOKEBCVJ")
DEFAULT_KEYMAP = {
    "menu_key": "G",
    "exit_key": "]",
    "chip_reset_key": "Z",
    "chip_reset_bootloader_key": "X",
    "toggle_output_key": "Q",
    "toggle_log_format_key": "M",
    "toggle_timestamp_key": "[",
    "dump_key": "\\",
}


def build_monitor_cfg(path, keymap=None):
    """Schreibt eine Monitor-Config, die die Browser-Shortcuts umgeht.

    Browser belegen Ctrl+T/R/P/F/L/I/H für eigene Funktionen; die einzige
    wirklich freie Kombination ist Ctrl+]. Deshalb bekommen alle Funktionen
    Tasten, die der Browser nicht abfaengt (Buchstaben ohne Browser-Bindung
    bzw. `]`). `exit_key` bleibt `]`, damit der Nutzer den Monitor gewohnt
    beenden kann.
    """
    keymap = dict(DEFAULT_KEYMAP if keymap is None else keymap)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="ascii") as fh:
        fh.write("[esp-idf-monitor]\n")
        for k, v in keymap.items():
            fh.write(f"{k} = {v}\n")
    return path


def parse_token_arg(path, query):
    """Token aus Query ODER aus dem Authorization-Header-Kontext.

    Nur der Query-Weg ist hier relevant (Browser-WebSocket kann keine
    Header setzen); die Funktion bleibt trotzdem testbar.
    """
    if query:
        params = urllib.parse.parse_qs(query)
        vals = params.get("token") or []
        if vals:
            return vals[0]
    return ""


def check_token(given, expected):
    if not expected:
        return True  # kein Token konfiguriert -> Auth ueber Reverse-Proxy
    return bool(given) and hmac.compare_digest(given, expected)


# --------------------------------------------------------------------------
# Fan-out: ein Monitor-Prozess, viele WebSocket-Clients.
# --------------------------------------------------------------------------
class Client:
    """Ein Browser-Client mit eigener Ausgabeschlange und Sender-Thread.

    Ohne eigene Schlange wuerde ein langsamer Tab den Reader-Thread blockieren
    und damit ALLE Clients ausbremsen. Die Schlange ist begrenzt; bei Ueberlauf
    wird der aelteste Block verworfen (ein Terminal darf nie den Server lahmlegen).
    """

    def __init__(self, conn):
        self.conn = conn
        self.q = queue.Queue(maxsize=512)
        self.dropped = 0
        self._stop = threading.Event()
        self._writer = threading.Thread(target=self._run, daemon=True)
        self._writer.start()

    def push(self, data: bytes):
        try:
            self.q.put_nowait(data)
        except queue.Full:
            try:
                self.q.get_nowait()  # aeltesten Block verwerfen
                self.dropped += 1
                self.q.put_nowait(data)
            except queue.Empty:
                pass

    def _run(self):
        while not self._stop.is_set():
            try:
                data = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            self.conn.send(data)

    def stop(self):
        self._stop.set()


class MonitorBridge:
    def __init__(self, cmd, env, cols=DEFAULT_COLS, rows=DEFAULT_ROWS,
                 scrollback_bytes=64 << 10):
        self.cmd = cmd
        self.env = env
        self.clients = []
        self.lock = threading.Lock()
        self.cols, self.rows = cols, rows
        self.master = None
        self.proc = None
        self._running = True
        self._scrollback = bytearray()
        self._scrollback_max = scrollback_bytes
        self._start()

    def _start(self):
        master, slave = pty.openpty()
        self._set_winsize(master, self.rows, self.cols)
        self.proc = subprocess.Popen(
            self.cmd, stdin=slave, stdout=slave, stderr=slave,
            env=self.env, close_fds=True, start_new_session=True,
        )
        os.close(slave)
        self.master = master

    def _set_winsize(self, fd, rows, cols):
        try:
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        except OSError:
            pass

    def add(self, conn):
        client = Client(conn)
        # Ein Tab, der spaeter/reconnectet, bekommt den bisherigen Verlauf, sonst
        # startet er mitten im Log (oder ganz leer, bevor der Monitor etwas sagt).
        with self.lock:
            backlog = bytes(self._scrollback)
            self.clients.append(client)
        if backlog:
            client.push(backlog)
        client.push(b"\r\n[web] verbunden. Ctrl+] beendet den Monitor.\r\n")
        return client

    def remove(self, client):
        client.stop()
        with self.lock:
            if client in self.clients:
                self.clients.remove(client)

    def broadcast(self, data: bytes):
        with self.lock:
            self._scrollback += data
            if len(self._scrollback) > self._scrollback_max:
                del self._scrollback[:len(self._scrollback) - self._scrollback_max]
            clients = list(self.clients)
        for c in clients:
            c.push(data)

    def write(self, data: bytes):
        if self.master is not None:
            try:
                os.write(self.master, data)
            except OSError:
                pass

    def resize(self, rows, cols):
        if not rows or not cols:
            return
        self.rows, self.cols = rows, cols
        if self.master is not None:
            self._set_winsize(self.master, rows, cols)

    def read_loop(self):
        """Liest das PTY und verteilt an alle Clients. Startet den Monitor neu,
        wenn er unerwartet endet (z. B. `make flash` im Monitor)."""
        while self._running:
            try:
                r, _, _ = select.select([self.master], [], [], 1.0)
            except OSError:
                break
            if r:
                try:
                    data = os.read(self.master, 4096)
                except OSError:
                    data = b""
                if data:
                    self.broadcast(data)
                    continue
                # EOF: Monitor beendet
            if self.proc.poll() is not None:
                code = self.proc.returncode
                self.broadcast(f"\r\n[web] Monitor beendet (exit {code}); "
                               f"Neustart in 2 s …\r\n".encode())
                if not self._running:
                    break
                time.sleep(2)
                try:
                    os.close(self.master)
                except OSError:
                    pass
                self._start()

    def stop(self):
        self._running = False
        try:
            if self.proc and self.proc.poll() is None:
                self.proc.terminate()
        except OSError:
            pass
        with self.lock:
            for c in self.clients:
                c.stop()
                c.conn.close()


# --------------------------------------------------------------------------
# HTTP/WS-Server
# --------------------------------------------------------------------------
def handle_client(sock, addr, bridge, token):
    try:
        conn, path, headers = wsproto.upgrade(sock)
    except wsproto.WSError:
        # Kein Upgrade: der Aufrufer hat uns nur fuer WS hierher geschickt, aber
        # der Handshake war kaputt. Sauber schliessen statt haengen.
        try:
            wsproto.serve_http(sock, "400 Bad Request",
                               b"expected a websocket upgrade\n")
        except OSError:
            sock.close()
        return

    parsed = urllib.parse.urlparse(path)
    if not parsed.path.startswith("/ws"):
        conn.close(1008)
        return
    given = parse_token_arg(path, parsed.query)
    if not check_token(given, token):
        conn.send_text(json.dumps({"type": "error", "msg": "unauthorized"}))
        conn.close(1008)
        return

    client = bridge.add(conn)
    dec = wsproto.FrameDecoder()
    try:
        while not conn.closed:
            try:
                data = conn.sock.recv(4096)
            except OSError:
                break
            if not data:
                break
            try:
                frames = dec.feed(data)
            except wsproto.WSError:
                break
            for opcode, payload in frames:
                if opcode == wsproto.OP_BINARY:
                    bridge.write(payload)
                elif opcode == wsproto.OP_TEXT:
                    _handle_text(bridge, payload)
                elif opcode == wsproto.OP_PING:
                    conn.send(payload, wsproto.OP_PONG)
                elif opcode == wsproto.OP_CLOSE:
                    conn.closed = True
                    break
    finally:
        bridge.remove(client)
        conn.close()


def _handle_text(bridge, payload):
    """Text-Frame: JSON-Steuernachricht ODER rohe Tastatureingabe.

    Browser senden Tasten normalerweise binaer; manche Clients schicken aber
    Text. Ein Text-Frame, der kein bekanntes JSON-Kommando ist, wird deshalb
    als Eingabe an den Monitor weitergereicht (statt verworfen).
    """
    try:
        msg = json.loads(payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        bridge.write(payload)
        return
    if isinstance(msg, dict) and msg.get("type") == "resize":
        bridge.resize(int(msg.get("rows") or 0), int(msg.get("cols") or 0))
    elif isinstance(msg, dict) and msg.get("type") == "input":
        bridge.write(str(msg.get("data", "")).encode("utf-8"))
    else:
        bridge.write(payload)


INDEX_HTML = b"""<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 IDF-Monitor</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/css/xterm.min.css">
<style>html,body{margin:0;height:100%;background:#000}#term{height:100%;
padding:6px;box-sizing:border-box}#bar{position:fixed;right:8px;top:8px;
font:12px monospace;color:#888;background:#111a;padding:2px 6px;border-radius:4px}
#conn{color:#e33}</style></head>
<body><div id="term"></div><div id="bar"><span id="conn">verbinde...</span></div>
<script src="https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/lib/xterm.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/@xterm/addon-fit@0.10.0/lib/addon-fit.min.js"></script>
<script>
const term=new Terminal({convertEol:false,scrollback:5000,fontSize:14,
  fontFamily:"Menlo,Consolas,monospace",theme:{background:"#000000"}});
const fit=new FitAddon.FitAddon();term.loadAddon(fit);term.open(document.getElementById('term'));
function refit(){try{fit.fit();}catch(e){}
  if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:"resize",rows:term.rows,cols:term.cols}));}
window.addEventListener('resize',refit);
const token=new URLSearchParams(location.search).get('token')||'';
const proto=location.protocol==='https:'?'wss':'ws';
let ws;
function connect(){
  ws=new WebSocket(`${proto}://${location.host}/ws?token=${encodeURIComponent(token)}`);
  ws.binaryType='arraybuffer';
  ws.onopen=()=>{document.getElementById('conn').textContent='verbunden';
    document.getElementById('conn').style.color='#3c3';refit();
    term.focus();};
  ws.onclose=()=>{document.getElementById('conn').textContent='getrennt - neu in 2 s';
    document.getElementById('conn').style.color='#e33';setTimeout(connect,2000);};
  ws.onmessage=(ev)=>{
    if(typeof ev.data==='string'){term.write(ev.data);return;}
    const s=new Uint8Array(ev.data);let out='';
    for(let i=0;i<s.length;i++)out+=String.fromCharCode(s[i]);
    term.write(out);
  };
}
term.onData(d=>{if(ws&&ws.readyState===1)ws.send(new TextEncoder().encode(d));});
connect();
</script></body></html>
"""


def serve(http_port, bridge, token, bind):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind, http_port))
    srv.listen(16)
    print(f"[idf-monitor-web] http://{bind}:{http_port}/  (WS: /ws?token=…)")
    while True:
        try:
            sock, addr = srv.accept()
        except OSError:
            break
        # Vorab-Peek: ist es ein HTTP-Upgrade? Dann WS, sonst Index.
        try:
            sock.settimeout(10)
            peek = sock.recv(4096, socket.MSG_PEEK)
        except OSError:
            sock.close()
            continue
        if b"Upgrade: websocket" in peek or b"upgrade: websocket" in peek:
            threading.Thread(target=handle_client,
                             args=(sock, addr, bridge, token), daemon=True).start()
        else:
            # Index bzw. 404. Query-Token fuer die Seite wird nur angezeigt.
            first = peek.split(b"\r\n", 1)[0].decode("latin-1", "replace")
            path = first.split(" ")[1] if " " in first else "/"
            if path.startswith("/") and not path.startswith("/ws"):
                try:
                    wsproto.serve_http(sock, "200 OK", INDEX_HTML,
                                       "text/html; charset=utf-8")
                except OSError:
                    sock.close()
            else:
                try:
                    wsproto.serve_http(sock, "404 Not Found", b"not found\n")
                except OSError:
                    sock.close()


def load_token(cli_token):
    if cli_token:
        return cli_token
    path = os.environ.get("IDF_MONITOR_WEB_TOKEN_FILE", "")
    if path and os.path.exists(path):
        val = open(path).read().strip()
        if val:
            return val
    return os.environ.get("IDF_MONITOR_WEB_TOKEN", "").strip()


def main(argv=None):
    ap = argparse.ArgumentParser(description="Serieller IDF-Monitor im Browser")
    ap.add_argument("--port", default="auto", help="serial port oder 'auto'")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--http-port", type=int,
                    default=int(os.environ.get("IDF_MONITOR_WEB_PORT", "7681")))
    ap.add_argument("--bind", default=os.environ.get("IDF_MONITOR_WEB_BIND", "0.0.0.0"))
    ap.add_argument("--token", default="", help="Zugangstoken (sonst $IDF_MONITOR_WEB_TOKEN)")
    ap.add_argument("--monitor", default=os.environ.get("IDF_MONITOR_BIN", "esp-idf-monitor"),
                    help="Pfad zu esp-idf-monitor")
    ap.add_argument("--cfg", default="", help="eigene Monitor-CFG (sonst automatisch)")
    ap.add_argument("--no-restart", action="store_true")
    args = ap.parse_args(argv)

    port = resolve_port(args.port)
    token = load_token(args.token)

    cfg = args.cfg or os.path.join(CFG_DIR, "esp-idf-monitor.cfg")
    build_monitor_cfg(cfg)

    env = dict(os.environ)
    env["ESP_IDF_MONITOR_CFGFILE"] = cfg
    cmd = [args.monitor, "-p", port, "-b", str(args.baud)]

    print(f"[idf-monitor-web] Port   : {port}")
    print(f"[idf-monitor-web] Befehl : {' '.join(cmd)}")
    print(f"[idf-monitor-web] CFG    : {cfg}")

    try:
        bridge = MonitorBridge(cmd, env)
    except FileNotFoundError:
        raise SystemExit(f"Monitor-Programm nicht gefunden: {args.monitor!r}. "
                         "Installiere esp-idf-monitor (pip) oder setze --monitor.")
    reader = threading.Thread(target=bridge.read_loop, daemon=True)
    reader.start()

    def _sig(_s, _f):
        bridge.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)
    serve(args.http_port, bridge, token, args.bind)


if __name__ == "__main__":
    main()
