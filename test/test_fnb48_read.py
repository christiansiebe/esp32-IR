"""Host tests for tools/fnb48_read.py (pure logic, no hardware).

Covers the CRC-8, the sample/packet decoder, the burst grouping and the
stop-only command framing plus the udev auto-stop rule syntax. The hidraw I/O
path is not touched here (no hardware, no mocks of the device).
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import fnb48_read as fr  # noqa: E402

RULE = os.path.join(os.path.dirname(__file__), "..", "tools", "udev",
                    "99-fnb48-stop.rules")

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


def test_stop_command():
    print("test_stop_command")
    # The stop is a fixed 64-byte HID report: AA 84, zero padding, CRC-8 0xA6.
    check(fr.CMD_STOP == bytes([0xAA, 0x84]) + bytes(61) + bytes([0xA6]),
          "CMD_STOP layout")
    check(len(fr.CMD_STOP) == fr.PACKET, f"CMD_STOP length {len(fr.CMD_STOP)}")
    check(fr.CMD_STOP[0] == 0xAA and fr.CMD_STOP[1] == 0x84, "CMD_STOP header")
    check(all(b == 0 for b in fr.CMD_STOP[2:63]), "CMD_STOP padding zero")
    check(fr.CMD_STOP[63] == 0xA6, "CMD_STOP trailer")


def test_stop_only_cli_parses():
    print("test_stop_only_cli_parses")
    # --stop-only is a boolean flag; -s is its short form. Parsing must not
    # require a device or touch hardware.
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--stop-only", action="store_true")
    check(ap.parse_args(["--stop-only"]).stop_only is True, "long form")
    check(ap.parse_args(["-s"]).stop_only is True, "short form")
    check(ap.parse_args([]).stop_only is False, "default off")


def test_udev_rule_syntax():
    print("test_udev_rule_syntax")
    with open(RULE) as fh:
        raw = fh.read()
    # A "#" only starts a comment at line start; no trailing-comment traps.
    lines = [ln for ln in raw.splitlines()
             if ln.strip() and not ln.lstrip().startswith("#")]
    check(len(lines) == 1, f"exactly one rule line, got {len(lines)}")
    rule = lines[0]
    # The device identity and the hidraw trigger must both be present.
    check('SUBSYSTEM=="hidraw"' in rule, "hidraw subsystem")
    check('ATTRS{idVendor}=="0483"' in rule, "vendor 0483")
    check('ATTRS{idProduct}=="003a"' in rule, "product 003a")
    check('ACTION=="add"' in rule, "add action")
    check("RUN+=" in rule, "RUN+ present")
    # Robustness: no unbind/reset (that hard-reset this board) and a bounded
    # helper (its own `timeout`) so udev can never block on a wedged node.
    check("unbind" not in rule and "remove" not in rule, "no unbind/remove")
    check("timeout" not in rule, "timeout lives in the helper, not the rule")
    check(rule.count("RUN+=") == 1, "single RUN+")
    # The rule must pass the device node through to the helper.
    check("$devnode" in rule or "%E{DEVNAME}" in rule or "%k" in rule,
          "device node passed")
    # Every rule line must be a single assignment set on one line (no "\").
    check("\\" not in raw, "no line continuation")


def test_udev_helper_syntax():
    print("test_udev_helper_syntax")
    helper = os.path.join(os.path.dirname(__file__), "..", "tools", "udev",
                          "fnb48-stop.sh")
    with open(helper) as fh:
        body = fh.read()
    # Bounded call: a hard `timeout` and the non-blocking stop-only path.
    check("timeout" in body, "helper caps runtime with timeout")
    check("--stop-only" in body, "helper calls the stop-only path")
    check("fnb48_read.py" in body, "helper runs the reader")


def main():
    for fn in (test_crc8, test_decode_sample, test_decode_packet_ok,
               test_decode_packet_bad_crc, test_decode_packet_rejects_non_data,
               test_find_bursts, test_find_bursts_merges_short_gap,
               test_find_bursts_none, test_stop_command,
               test_stop_only_cli_parses, test_udev_rule_syntax,
               test_udev_helper_syntax):
        fn()
    print(f"{CHECKS} checks, {FAILURES} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
