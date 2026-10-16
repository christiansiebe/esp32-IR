#!/usr/bin/env python3
"""Minimaler, stdlib-only WebSocket-Server (RFC 6455) fuer den IDF-Monitor-Web.

Warum selbst gebaut: der Odroid soll nur `esp-idf-monitor` brauchen. `ttyd` hat
kein Debian-arm64-Paket und `websockets`/`pyserial` wuerden ein zweites Venv
erzwingen. Dieses Modul deckt genau das ab, was ein Terminal im Browser
braucht: HTTP-Upgrade, maskierte Client-Frames (Text/Binaer), Fragmente,
Ping/Pong und Close. Kein TLS (dafuer Reverse-Proxy davor).

Reine Codec-Funktionen (`accept_key`, `encode_frame`, `FrameDecoder`) sind
host-testbar ohne Socket, siehe test/test_idf_monitor_web.py.
"""

import base64
import hashlib
import socket
import struct
import threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT = 0x0
OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA


class WSError(Exception):
    pass


def accept_key(client_key: str) -> str:
    """Sec-WebSocket-Accept = base64(sha1(key + GUID))."""
    if not client_key:
        raise WSError("missing Sec-WebSocket-Key")
    digest = hashlib.sha1((client_key + GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def encode_frame(payload: bytes, opcode: int = OP_BINARY) -> bytes:
    """Server-zu-Client-Frame. Server maskieren nie."""
    header = bytearray()
    header.append(0x80 | (opcode & 0x0F))  # FIN + opcode
    n = len(payload)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header.append(126)
        header += struct.pack("!H", n)
    else:
        header.append(127)
        header += struct.pack("!Q", n)
    return bytes(header) + payload


class FrameDecoder:
    """Inkrementeller Server-seitiger Frame-Decoder.

    Erwartet maskierte Client-Frames (Pflicht laut RFC). Gibt bei jedem
    vollstaendigen Datenframe `(opcode, payload)` zurueck; Steuerframes
    (Ping/Pong/Close) werden ebenso gemeldet, aber fragmentierte Datenframes
    werden bereits hier zusammengesetzt.
    """

    def __init__(self, max_frame: int = 8 << 20):
        self._buf = bytearray()
        self._max_frame = max_frame
        self._frag_op = None
        self._frag = bytearray()

    def feed(self, data: bytes):
        self._buf += data
        out = []
        while True:
            frame = self._try_parse()
            if frame is None:
                break
            fin, opcode, payload = frame
            if opcode >= 0x8:  # Steuerframe: immer eigenstaendig, nie fragmentiert
                out.append((opcode, payload))
                continue
            if opcode == OP_CONT:
                if self._frag_op is None:
                    raise WSError("CONT without start")
                self._frag += payload
                if fin:
                    out.append((self._frag_op, bytes(self._frag)))
                    self._frag_op = None
                    self._frag = bytearray()
            else:
                if fin:
                    out.append((opcode, payload))
                else:
                    self._frag_op = opcode
                    self._frag = bytearray(payload)
        return out

    def _try_parse(self):
        buf = self._buf
        if len(buf) < 2:
            return None
        b0, b1 = buf[0], buf[1]
        fin = bool(b0 & 0x80)
        opcode = b0 & 0x0F
        masked = bool(b1 & 0x80)
        n = b1 & 0x7F
        idx = 2
        if n == 126:
            if len(buf) < idx + 2:
                return None
            n = struct.unpack_from("!H", buf, idx)[0]
            idx += 2
        elif n == 127:
            if len(buf) < idx + 8:
                return None
            n = struct.unpack_from("!Q", buf, idx)[0]
            idx += 8
        if n > self._max_frame:
            raise WSError(f"frame too large: {n}")
        if not masked:
            raise WSError("client frame not masked")
        if len(buf) < idx + 4:
            return None
        mask = buf[idx:idx + 4]
        idx += 4
        if len(buf) < idx + n:
            return None
        payload = bytearray(buf[idx:idx + n])
        for i in range(n):
            payload[i] ^= mask[i & 3]
        del buf[:idx + n]
        return fin, opcode, bytes(payload)


class Connection:
    """Eine angenommene WebSocket-Verbindung mit eigener Sende-Sperre."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self._send_lock = threading.Lock()
        self.closed = False

    def send(self, payload: bytes, opcode: int = OP_BINARY):
        if self.closed:
            return
        try:
            with self._send_lock:
                self.sock.sendall(encode_frame(payload, opcode))
        except OSError:
            self.closed = True

    def send_text(self, text: str):
        self.send(text.encode("utf-8"), OP_TEXT)

    def close(self, code: int = 1000):
        if self.closed:
            return
        try:
            with self._send_lock:
                self.sock.sendall(encode_frame(struct.pack("!H", code), OP_CLOSE))
        except OSError:
            pass
        finally:
            self.closed = True
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass


def _read_http_head(sock: socket.socket, limit: int = 16 << 10) -> bytes:
    data = bytearray()
    sock.settimeout(10)
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        if len(data) > limit:
            raise WSError("HTTP head too large")
    return bytes(data)


def upgrade(sock: socket.socket):
    """Liest den HTTP-Upgrade-Request und antwortet mit 101.

    Liefert `(Connection, path, headers)`. Bei fehlendem Upgrade-Header wird
    `WSError` geworfen (der Aufrufer kann dann noch eine normale HTTP-Antwort
    senden).
    """
    raw = _read_http_head(sock)
    head, _, _rest = raw.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    if not lines:
        raise WSError("empty request")
    request_line = lines[0].decode("latin-1", "replace")
    parts = request_line.split()
    path = parts[1] if len(parts) >= 2 else "/"
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, v = line.split(b":", 1)
            headers[k.strip().decode("latin-1").lower()] = v.strip().decode("latin-1")
    if headers.get("upgrade", "").lower() != "websocket":
        raise WSError("not a websocket upgrade")
    key = headers.get("sec-websocket-key", "")
    accept = accept_key(key)
    resp = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    sock.sendall(resp.encode("ascii"))
    sock.settimeout(None)
    return Connection(sock), path, headers


def serve_http(sock: socket.socket, status: str, body: bytes,
               content_type: str = "text/plain; charset=utf-8",
               extra_headers=None):
    headers = [
        f"HTTP/1.1 {status}",
        f"Content-Type: {content_type}",
        f"Content-Length: {len(body)}",
        "Connection: close",
    ]
    if extra_headers:
        headers += extra_headers
    head = "\r\n".join(headers) + "\r\n\r\n"
    sock.sendall(head.encode("ascii") + body)
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    sock.close()
