#!/usr/bin/env python3
"""Host-Tests fuer idf-monitor-web. Ohne Hardware, ohne Netz, ohne Mocks.

Getestet werden die reinen, entscheidenden Teile:
  - WS-Codec (RFC 6455: Accept-Key, Frame-Encode, maskierte Client-Frames,
    Fragmente, Steuerframes) — die Grundlage jeder Browser-Verbindung.
  - Port-Erkennung/-Filter: Board-Konsole (ttyAML/ttyS) MUSS abgelehnt werden.
  - Auth-Vergleich (konstantzeitig, leeres Token = kein Zwang).
  - Monitor-CFG: umgeht die Browser-Shortcuts.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "remote-shell", "idf-monitor-web")
sys.path.insert(0, os.path.abspath(TOOL))

import idf_monitor_web as m  # noqa: E402
import wsproto  # noqa: E402

CHECKS = 0
FAILS = 0


def check(name, cond):
    global CHECKS, FAILS
    CHECKS += 1
    if cond:
        print(f"  ok   {name}")
    else:
        FAILS += 1
        print(f"  FAIL {name}")


def client_frame(payload: bytes, opcode: int = wsproto.OP_BINARY,
                 fin: bool = True, mask: bytes = b"\x01\x02\x03\x04") -> bytes:
    """Baut einen maskierten Client-Frame (wie ein Browser ihn sendet)."""
    b0 = (0x80 if fin else 0) | opcode
    n = len(payload)
    out = bytearray([b0])
    if n < 126:
        out.append(0x80 | n)
    elif n < 65536:
        out.append(0x80 | 126)
        out += struct.pack("!H", n)
    else:
        out.append(0x80 | 127)
        out += struct.pack("!Q", n)
    out += mask
    for i in range(n):
        out.append(payload[i] ^ mask[i & 3])
    return bytes(out)


def test_accept_key():
    # Bekannter Vektor aus RFC 6455 §1.3.
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    check("accept_key RFC-Vektor",
          wsproto.accept_key(key) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
    try:
        wsproto.accept_key("")
        check("leerer Key -> Fehler", False)
    except wsproto.WSError:
        check("leerer Key -> Fehler", True)


def test_encode_frame():
    f = wsproto.encode_frame(b"hi", wsproto.OP_TEXT)
    check("kleiner Frame FIN+Text", f[0] == (0x80 | 0x1))
    check("kleiner Frame Laenge unmaskiert", f[1] == 2)
    check("kleiner Frame Payload", f[2:] == b"hi")

    big = b"x" * 300
    f = wsproto.encode_frame(big, wsproto.OP_BINARY)
    check("mittlerer Frame 126-Marker", f[1] == 126)
    check("mittlerer Frame Laenge", struct.unpack("!H", f[2:4])[0] == 300)
    check("mittlerer Frame Payload", f[4:] == big)


def test_decoder_simple():
    dec = wsproto.FrameDecoder()
    out = dec.feed(client_frame(b"hello", wsproto.OP_TEXT))
    check("ein Frame -> ein Ergebnis", len(out) == 1)
    check("Text-Opcode", out[0][0] == wsproto.OP_TEXT)
    check("Payload korrekt", out[0][1] == b"hello")


def test_decoder_split():
    """Ein Frame, in zwei TCP-Segmenten -> erst nach dem zweiten vollstaendig."""
    raw = client_frame(b"world", wsproto.OP_BINARY)
    dec = wsproto.FrameDecoder()
    check("halber Frame -> nichts", dec.feed(raw[:3]) == [])
    out = dec.feed(raw[3:])
    check("Rest -> vollstaendig", out == [(wsproto.OP_BINARY, b"world")])


def test_decoder_two_in_one():
    raw = client_frame(b"a", wsproto.OP_BINARY) + client_frame(b"b", wsproto.OP_BINARY)
    dec = wsproto.FrameDecoder()
    out = dec.feed(raw)
    check("zwei Frames in einem Puffer", out == [(wsproto.OP_BINARY, b"a"),
                                                 (wsproto.OP_BINARY, b"b")])


def test_decoder_fragments():
    """Fragmentierter Text: TEXT(no-fin) + CONT(fin) -> ein Textframe."""
    dec = wsproto.FrameDecoder()
    part1 = client_frame(b"Hel", wsproto.OP_TEXT, fin=False)
    part2 = client_frame(b"lo", wsproto.OP_CONT, fin=True)
    check("Fragment 1 -> nichts", dec.feed(part1) == [])
    out = dec.feed(part2)
    check("Fragmente zusammengesetzt", out == [(wsproto.OP_TEXT, b"Hello")])


def test_decoder_control_and_unmasked():
    dec = wsproto.FrameDecoder()
    out = dec.feed(client_frame(b"p", wsproto.OP_PING))
    check("Ping gemeldet", out == [(wsproto.OP_PING, b"p")])
    # Unmaskierter Client-Frame ist ein Protokollfehler.
    bad = bytes([0x80 | wsproto.OP_TEXT, 0x01]) + b"x"
    try:
        dec.feed(bad)
        check("unmaskierter Client-Frame -> Fehler", False)
    except wsproto.WSError:
        check("unmaskierter Client-Frame -> Fehler", True)


def test_port_filter():
    check("ttyUSB erlaubt", not m.FORBIDDEN.search("ttyUSB0"))
    check("ttyACM erlaubt", not m.FORBIDDEN.search("ttyACM0"))
    check("ttyAML0 (Board-Konsole) gesperrt", bool(m.FORBIDDEN.search("ttyAML0")))
    check("ttyS0 gesperrt", bool(m.FORBIDDEN.search("ttyS0")))
    try:
        m.resolve_port("/dev/ttyAML0")
        check("resolve_port lehnt Board-Konsole ab", False)
    except SystemExit:
        check("resolve_port lehnt Board-Konsole ab", True)


def test_auth():
    check("richtiges Token", m.check_token("abc", "abc"))
    check("falsches Token", not m.check_token("x", "abc"))
    check("leeres Token bei fehlendem Secret erlaubt", m.check_token("", ""))
    check("leeres Token bei gesetztem Secret abgelehnt", not m.check_token("", "abc"))
    # Token aus der Query
    check("parse_token_arg", m.parse_token_arg("/ws", "token=zzz") == "zzz")
    check("parse_token_arg leer", m.parse_token_arg("/ws", "") == "")


def test_monitor_cfg(tmpdir="/tmp/idf-moncfg-test"):
    path = os.path.join(tmpdir, "m.cfg")
    m.build_monitor_cfg(path)
    text = open(path, encoding="ascii").read()
    check("CFG hat Sektion", "[esp-idf-monitor]" in text)
    check("exit_key bleibt ]", "exit_key = ]" in text)
    # Kein Key darf ein Browser-Shortcut sein, sonst kommt die Taste nie an.
    for line in text.splitlines():
        if "=" in line and not line.startswith("["):
            name, key = (p.strip() for p in line.split("=", 1))
            check(f"Key kein Browser-Shortcut: {name}={key}",
                  key not in m.BROWSER_BOUND)
            # Alle Keys muessen als C0 darstellbar sein.
            check(f"Key darstellbar: {name}={key}", len(key) == 1)


def main():
    for fn in (test_accept_key, test_encode_frame, test_decoder_simple,
               test_decoder_split, test_decoder_two_in_one,
               test_decoder_fragments, test_decoder_control_and_unmasked,
               test_port_filter, test_auth, test_monitor_cfg):
        print(fn.__name__)
        fn()
    print(f"\n{CHECKS} Checks, {FAILS} Fehler")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
