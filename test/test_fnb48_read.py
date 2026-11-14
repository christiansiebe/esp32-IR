"""Host tests for tools/fnb48_read.py (pure logic, no hardware).

Covers the CRC-8, the sample/packet decoder and the burst grouping. The
hidraw I/O path is not touched here.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import fnb48_read as fr  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def make_sample(voltage, current, dp=0.0, dn=0.0, temp=20.0):
    return struct.pack(
        "<IIHHB H", int(voltage * 100000), int(current * 100000),
        int(dp * 1000), int(dn * 1000), 0x01, int(temp * 10))


def make_packet(samples, crc_ok=True):
    body = bytearray()
    for s in samples:
        body += s
    body += bytes(61 - len(body))                   # pad to byte 62 (CRC is byte 63)
    head = bytes([0xAA, 0x04]) + bytes(body)         # 63 bytes
    crc = fr.crc8(head[1:63])
    if not crc_ok:
        crc ^= 0xFF
    return head + bytes([crc])


def test_crc8():
    print("test_crc8")
    # Known check value for poly 0x39 / init 0x42 over "123456789".
    check(fr.crc8(b"123456789") == 0x4B, "crc8 check value")


def test_decode_sample():
    print("test_decode_sample")
    s = fr.decode_sample(make_sample(5.19, 0.0455, dp=3.15, dn=0.03, temp=20.5))
    check(abs(s["voltage"] - 5.19) < 1e-4, f"voltage {s['voltage']}")
    check(abs(s["current"] - 0.0455) < 1e-4, f"current {s['current']}")
    check(abs(s["power"] - 5.19 * 0.0455) < 1e-3, f"power {s['power']}")
    check(abs(s["dp"] - 3.15) < 1e-3, f"dp {s['dp']}")
    check(abs(s["dn"] - 0.03) < 1e-3, f"dn {s['dn']}")
    check(abs(s["temp"] - 20.5) < 1e-3, f"temp {s['temp']}")


def test_decode_packet_ok():
    print("test_decode_packet_ok")
    samples = [make_sample(5.0, 0.01 * i) for i in range(1, 5)]
    got, ok = fr.decode_packet(make_packet(samples))
    check(len(got) == 4, f"sample count {len(got)}")
    check(ok is True, "crc ok flag")
    check(abs(got[3]["current"] - 0.04) < 1e-4, "last sample current")


def test_decode_packet_bad_crc():
    print("test_decode_packet_bad_crc")
    samples = [make_sample(5.0, 0.01)] * 4
    got, ok = fr.decode_packet(make_packet(samples, crc_ok=False))
    check(ok is False, "bad crc detected")
    check(len(got) == 4, "samples still decoded")


def test_decode_packet_rejects_non_data():
    print("test_decode_packet_rejects_non_data")
    got, ok = fr.decode_packet(bytes([0xAA, 0x03]) + bytes(62))
    check(got == [] and ok is None, "info packet ignored")
    got, ok = fr.decode_packet(b"\x00" * fr.PACKET)
    check(got == [] and ok is None, "non-AA packet ignored")


def test_find_bursts():
    print("test_find_bursts")
    # idle, one 3-sample burst, idle, another 2-sample burst.
    # max_gap=0.05 s so the 60 ms idle run (6 samples) splits them.
    powers = [0.06] * 5 + [0.2, 0.25, 0.2] + [0.06] * 5 + [0.3, 0.3] + [0.06] * 3
    bursts = fr.find_bursts(powers, threshold=0.15, max_gap=0.05)
    check(len(bursts) == 2, f"burst count {len(bursts)}")
    check(bursts[0][1] == 3 and abs(bursts[0][2] - 0.25) < 1e-9, "first burst")
    check(bursts[1][1] == 2 and abs(bursts[1][2] - 0.3) < 1e-9, "second burst")


def test_find_bursts_merges_short_gap():
    print("test_find_bursts_merges_short_gap")
    # A single idle sample inside the burst must not split it (gap 10 ms).
    powers = [0.2, 0.2, 0.06, 0.2, 0.2]
    bursts = fr.find_bursts(powers, threshold=0.15, max_gap=0.5)
    check(len(bursts) == 1, f"merged count {len(bursts)}")
    check(bursts[0][1] == 5, f"merged length {bursts[0][1]}")


def test_find_bursts_none():
    print("test_find_bursts_none")
    check(fr.find_bursts([0.06, 0.07, 0.05], 0.15) == [], "no bursts")


def main():
    for fn in (test_crc8, test_decode_sample, test_decode_packet_ok,
               test_decode_packet_bad_crc, test_decode_packet_rejects_non_data,
               test_find_bursts, test_find_bursts_merges_short_gap,
               test_find_bursts_none):
        fn()
    print(f"{CHECKS} checks, {FAILURES} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
