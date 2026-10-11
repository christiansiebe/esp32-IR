"""Host tests for tools/ws_min.py framing (pure, no sockets)."""

import os
import sys

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


def test_accept_key():
    print("test_accept_key")
    # Known RFC 6455 example.
    check(ws_min.expected_accept("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "accept key matches RFC example")


def test_round_trip_small():
    print("test_round_trip_small")
    payload = b"hello"
    frame = ws_min.encode_frame(payload)
    check(frame[0] == 0x81, "FIN + text opcode")
    check(frame[1] & 0x80, "client frame is masked")
    decoded = ws_min.decode_frame(frame)
    check(decoded is not None, "frame decodes")
    opcode, out, consumed = decoded
    check(opcode == ws_min.OPCODE_TEXT, "opcode preserved")
    check(out == payload, "payload round-trips")
    check(consumed == len(frame), "consumed length is exact")


def test_round_trip_medium_and_large():
    print("test_round_trip_medium_and_large")
    for size in (200, 70000):
        payload = bytes((i % 256) for i in range(size))
        frame = ws_min.encode_frame(payload)
        opcode, out, consumed = ws_min.decode_frame(frame)
        check(out == payload, "payload of %d bytes round-trips" % size)
        check(consumed == len(frame), "length of %d bytes is exact" % size)


def test_incomplete_returns_none():
    print("test_incomplete_returns_none")
    frame = ws_min.encode_frame(b"abcdef")
    check(ws_min.decode_frame(frame[:3]) is None, "partial header yields None")
    check(ws_min.decode_frame(frame[:-1]) is None, "partial payload yields None")


def test_server_frame_unmasked():
    print("test_server_frame_unmasked")
    # Server frames are not masked: FIN+text, length, payload.
    data = bytes([0x81, 0x03]) + b"abc"
    decoded = ws_min.decode_frame(data)
    check(decoded is not None, "unmasked frame decodes")
    opcode, out, consumed = decoded
    check(out == b"abc", "unmasked payload")
    check(consumed == 5, "unmasked consumed length")


def main():
    test_accept_key()
    test_round_trip_small()
    test_round_trip_medium_and_large()
    test_incomplete_returns_none()
    test_server_frame_unmasked()
    print("\n%d checks, %d failures" % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
