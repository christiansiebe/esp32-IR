"""Minimal RFC 6455 WebSocket client (text frames only).

Home Assistant exposes ``recorder/import_statistics`` only over its WebSocket
API, so the import bridge needs a WebSocket client. To avoid adding a
dependency (the CI image and the user's machine may not have ``websockets``),
this is a small, purpose-built implementation.

The framing helpers are pure and covered by test/test_ws_min.py; ``Client`` is
the thin socket wrapper used by tools/ha_import.py.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import struct

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OPCODE_TEXT = 0x1
OPCODE_CLOSE = 0x8
OPCODE_PING = 0x9
OPCODE_PONG = 0xA


def build_handshake(host: str, path: str, key: str) -> bytes:
    return (
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n" % (path, host, key)
    ).encode()


def expected_accept(key: str) -> str:
    digest = hashlib.sha1((key + GUID).encode()).digest()
    return base64.b64encode(digest).decode()


def encode_frame(payload: bytes, opcode: int = OPCODE_TEXT) -> bytes:
    """Encode a client frame; client frames must be masked."""
    mask = os.urandom(4)
    header = bytearray([0x80 | opcode])  # FIN set
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header += struct.pack("!H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack("!Q", length)
    header += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return bytes(header) + masked


def decode_frame(data: bytes):
    """Decode one server frame.

    Returns (opcode, payload, consumed_bytes) or None when the buffer does not
    yet hold a complete frame.
    """
    if len(data) < 2:
        return None
    opcode = data[0] & 0x0F
    masked = data[1] & 0x80
    length = data[1] & 0x7F
    index = 2
    if length == 126:
        if len(data) < 4:
            return None
        length = struct.unpack("!H", data[2:4])[0]
        index = 4
    elif length == 127:
        if len(data) < 10:
            return None
        length = struct.unpack("!Q", data[2:10])[0]
        index = 10
    mask = b""
    if masked:
        if len(data) < index + 4:
            return None
        mask = data[index:index + 4]
        index += 4
    if len(data) < index + length:
        return None
    payload = data[index:index + length]
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload, index + length


class Client:
    """Tiny blocking WebSocket client good enough for a request/response flow."""

    def __init__(self, host: str, port: int, path: str = "/api/websocket",
                 timeout: float = 15.0):
        self.host = host
        self.port = port
        self.path = path
        self.timeout = timeout
        self._buffer = b""
        self._sock = None

    def connect(self):
        self._sock = socket.create_connection((self.host, self.port), self.timeout)
        self._sock.settimeout(self.timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self._sock.sendall(build_handshake(self.host, self.path, key))
        response = self._read_http_headers()
        status = response.split("\r\n", 1)[0]
        if "101" not in status:
            raise RuntimeError("WebSocket handshake failed: %s" % status)
        accept = None
        for line in response.split("\r\n"):
            if line.lower().startswith("sec-websocket-accept:"):
                accept = line.split(":", 1)[1].strip()
        if accept != expected_accept(key):
            raise RuntimeError("WebSocket handshake returned a wrong accept key")
        return self

    def _read_http_headers(self) -> str:
        while b"\r\n\r\n" not in self._buffer:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed during handshake")
            self._buffer += chunk
        header, _, rest = self._buffer.partition(b"\r\n\r\n")
        self._buffer = rest
        return header.decode("latin-1")

    def _recv_frame(self):
        while True:
            frame = decode_frame(self._buffer)
            if frame is not None:
                opcode, payload, consumed = frame
                self._buffer = self._buffer[consumed:]
                return opcode, payload
            chunk = self._sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed by peer")
            self._buffer += chunk

    def send_json(self, obj: dict):
        self._sock.sendall(encode_frame(json.dumps(obj).encode("utf-8")))

    def recv_json(self) -> dict:
        while True:
            opcode, payload = self._recv_frame()
            if opcode == OPCODE_CLOSE:
                raise RuntimeError("server closed the WebSocket")
            if opcode == OPCODE_PING:
                self._sock.sendall(encode_frame(payload, OPCODE_PONG))
                continue
            if opcode == OPCODE_TEXT:
                return json.loads(payload.decode("utf-8"))

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
