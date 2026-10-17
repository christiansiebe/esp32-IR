"""Integration test for tools/ws_min.Client against a real local WebSocket
server (stdlib only, no mocks): exercises the handshake, masking, framing and
the auth + import_statistics request/response flow that ha_import.py uses."""

import json
import os
import socket
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import ws_min  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def serve_once(listener, reply_sequence, captured):
    conn, _ = listener.accept()
    buffer = b""
    while b"\r\n\r\n" not in buffer:
        buffer += conn.recv(4096)
    headers = buffer.split(b"\r\n\r\n", 1)[0].decode("latin-1")
    key = ""
    for line in headers.split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    accept = ws_min.expected_accept(key)
    conn.sendall(
        ("HTTP/1.1 101 Switching Protocols\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode()
    )
    rest = buffer.split(b"\r\n\r\n", 1)[1]
    for index, reply in enumerate(reply_sequence):
        conn.sendall(ws_min.encode_frame(json.dumps(reply).encode()))
        if index == len(reply_sequence) - 1:
            # No reply follows the final message, so do not wait for one.
            break
        # Read the client's next message.
        frame = None
        while frame is None:
            if not rest:
                rest = conn.recv(4096)
                if not rest:
                    break
            decoded = ws_min.decode_frame(rest)
            if decoded is None:
                rest += conn.recv(4096)
                continue
            _, payload, consumed = decoded
            rest = rest[consumed:]
            frame = json.loads(payload.decode())
        captured.append(frame)
    conn.close()


def test_client_auth_and_import():
    print("test_client_auth_and_import")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    replies = [
        {"type": "auth_required", "ha_version": "2026.1.0"},
        {"type": "auth_ok", "ha_version": "2026.1.0"},
        {"id": 1, "type": "result", "success": True, "result": None},
    ]
    captured = []
    thread = threading.Thread(target=serve_once, args=(listener, replies, captured))
    thread.start()

    client = ws_min.Client("127.0.0.1", port).connect()
    greeting = client.recv_json()
    check(greeting["type"] == "auth_required", "greeting received")
    client.send_json({"type": "auth", "access_token": "token"})
    check(client.recv_json()["type"] == "auth_ok", "auth accepted")
    message = {
        "id": 1,
        "type": "recorder/import_statistics",
        "metadata": {"statistic_id": "esp32ir:meter_energy"},
        "stats": [{"start": "2024-01-01T00:00:00+00:00", "sum": 1.0, "state": 1.0}],
    }
    client.send_json(message)
    result = client.recv_json()
    check(result["success"] is True, "import acknowledged")
    client.close()
    thread.join(timeout=5)

    check(len(captured) == 2, "server saw auth + import messages")
    check(captured[0]["type"] == "auth", "first message was auth")
    check(captured[1]["type"] == "recorder/import_statistics", "second was import")
    check(captured[1]["stats"][0]["sum"] == 1.0, "payload survived the round trip")
    listener.close()


def main():
    test_client_auth_and_import()
    print("\n%d checks, %d failures" % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
