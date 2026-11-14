#!/usr/bin/env python3
"""Read live values from a FNIRSI FNB-48 USB power meter via /dev/hidrawN.

Used to measure the real supply current of an ESP32 board (e.g. the C6
receiver) on the bench. Stdlib only (os/select/struct/time) - no pyusb, no
hidapi, no udev rules; the device is opened directly as a raw HID node.

Protocol (FNIRSI 64-byte HID reports, USB 0483:003a, HID, not a tty):

  Host -> device  (64 bytes, raw hidraw write, NO report id prepended):
    AA 81 00..00 8E   init / handshake
    AA 82 00..00 96   start the data stream
    AA 83 00..00 9E   keep-alive (repeat ~every 1 s while reading)
    AA 84 00..00 A6   STOP the data stream

  IMPORTANT: 0x84 (stop) MUST be sent after reading. Without it the meter's
  internal stream/logger keeps running, its buffer fills up and the live
  values freeze. A stop is also sent before each start to clear a stuck
  stream.

  Device -> host  (64 bytes):
    byte0 = 0xAA, byte1 = 0x04 (data packet) / 0x03 (info, ignored),
    byte63 = CRC-8 over byte1..62.
    A data packet carries 4 samples of 15 bytes each, starting at offset 2.
    Sample layout (relative offset within the sample):
      +0  uint32 LE  voltage / 100000  [V]
      +4  uint32 LE  current / 100000  [A]
      +8  uint16 LE  D+      / 1000    [V]
      +10 uint16 LE  D-      / 1000    [V]
      +12 uint8      constant 0x01
      +13 uint16 LE  temperature / 10  [degC]
    All little-endian. Power is NOT transmitted; it is computed as P = V * I.
    CRC-8: poly 0x39, init 0x42, no refin/refout, xorout 0x00 (check 0x4b).

The device samples at a fixed 100 S/s. Do not average over a short window if
you care about spikes: log every sample (``-o file.csv``) so short load bursts
survive. ``--burst`` groups consecutive samples above a power threshold into
bursts and reports their spacing and peak (e.g. the ~5 s radio bursts of a
sleeping ESP32-C6).

If /dev/hidraw0 is missing although the meter is plugged in, usbhid sometimes
fails to bind after a hub/plug event:

    echo 1-1.1:1.0 > /sys/bus/usb/drivers/usbhid/bind

Usage:
    python3 tools/fnb48_read.py -n 60 -o c6.csv
    python3 tools/fnb48_read.py -n 30 --burst 0.15
"""

import argparse
import os
import select
import signal
import struct
import sys
import time

PACKET = 64
SAMPLE = 15
SAMPLES_PER_PACKET = 4
SAMPLE_RATE = 100.0  # fixed sample rate of the FNB-48 [S/s]

CMD_INIT = bytes([0xAA, 0x81]) + bytes(61) + bytes([0x8E])
CMD_START = bytes([0xAA, 0x82]) + bytes(61) + bytes([0x96])
CMD_KEEPALIVE = bytes([0xAA, 0x83]) + bytes(61) + bytes([0x9E])
CMD_STOP = bytes([0xAA, 0x84]) + bytes(61) + bytes([0xA6])


def crc8(data):
    """CRC-8, poly 0x39, init 0x42 (FNIRSI data packets)."""
    crc = 0x42
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x39) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def decode_sample(buf):
    """Decode one 15-byte sample into a dict."""
    v_raw, i_raw = struct.unpack_from("<II", buf, 0)
    dp_raw, dn_raw = struct.unpack_from("<HH", buf, 8)
    temp_raw = struct.unpack_from("<H", buf, 13)[0]
    voltage = v_raw / 100000.0
    current = i_raw / 100000.0
    return {
        "voltage": voltage,
        "current": current,
        "power": voltage * current,
        "dp": dp_raw / 1000.0,
        "dn": dn_raw / 1000.0,
        "temp": temp_raw / 10.0,
    }


def decode_packet(buf):
    """64-byte report -> (samples, crc_ok). Empty list if not a data packet."""
    if len(buf) != PACKET or buf[0] != 0xAA or buf[1] != 0x04:
        return [], None
    crc_ok = crc8(buf[1:63]) == buf[63]
    samples = [
        decode_sample(buf[2 + i * SAMPLE: 2 + (i + 1) * SAMPLE])
        for i in range(SAMPLES_PER_PACKET)
    ]
    return samples, crc_ok


def _drain(fd, duration):
    """Discard all pending reports (flush the device FIFO)."""
    end = time.time() + duration
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            continue
        try:
            os.read(fd, PACKET)
        except (BlockingIOError, OSError):
            break


def _send_stop(fd):
    """Best-effort 0x84; never raises (fd may already be gone)."""
    try:
        os.write(fd, CMD_STOP)
    except OSError:
        pass


def read_samples(dev, duration=1.0, keepalive=1.0, on_sample=None):
    """Send init, read the stream, stop it again, return the samples.

    keepalive=1.0 s as in the reference CLI (fnb48s-cli transport.py): a too
    frequent 0x83 (e.g. every 3 ms) overruns the meter's command parser and
    contributes to it filling up.

    on_sample: optional callback(sample, t_rel) called for EVERY sample
    (for lossless live logging / CSV export).

    The 0x84 stop is sent on every exit path - normal end, exception, and
    SIGINT/SIGTERM - because a run that is killed without it leaves the meter's
    internal logger running, its buffer fills up and the live values freeze.
    That is the intermittent "freeze" seen when a long measurement is aborted.
    """
    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
    samples = []
    crc_seen = False
    crc_ok = True

    # Turn a terminating signal into KeyboardInterrupt so the finally block
    # below still runs the stop.
    def _on_signal(_signum, _frame):
        raise KeyboardInterrupt

    old_int = signal.signal(signal.SIGINT, _on_signal)
    old_term = signal.signal(signal.SIGTERM, _on_signal)
    try:
        # Stop a possibly stuck stream first and flush the FIFO. Twice, in case
        # an earlier run died mid-stream and left the parser wedged.
        for _ in range(2):
            _send_stop(fd)
            time.sleep(0.1)
            _drain(fd, 0.2)

        for frame in (CMD_INIT, CMD_START, CMD_KEEPALIVE):
            os.write(fd, frame)
            time.sleep(0.03)

        t0 = time.time()
        deadline = t0 + duration
        next_ka = 0.0
        while time.time() < deadline:
            now = time.time()
            if now >= next_ka:
                os.write(fd, CMD_KEEPALIVE)
                next_ka = now + keepalive
            r, _, _ = select.select([fd], [], [], 0.02)
            if not r:
                continue
            try:
                buf = os.read(fd, PACKET)
            except (BlockingIOError, OSError):
                continue
            got, ok = decode_packet(buf)
            if ok is not None:
                crc_seen = True
                crc_ok = crc_ok and ok
            if on_sample is not None:
                for s in got:
                    on_sample(s, time.time() - t0)
            samples.extend(got)
    finally:
        # Always stop the stream (0x84) and flush: otherwise the meter's
        # internal logger keeps running and the live values freeze.
        _send_stop(fd)
        _drain(fd, 0.4)
        signal.signal(signal.SIGINT, old_int)
        signal.signal(signal.SIGTERM, old_term)
        os.close(fd)
    return samples, (crc_seen, crc_ok)


def find_bursts(powers, threshold, max_gap=0.5):
    """Group consecutive samples above `threshold` into bursts.

    Returns a list of (start_index, length, peak_power). A dip of at most
    max_gap seconds does not split a burst. This finds short load spikes
    (e.g. radio bursts) that disappear inside an average.
    """
    dt = 1.0 / SAMPLE_RATE
    bursts = []
    start = last = None
    peak = 0.0
    for i, p in enumerate(powers):
        if p > threshold:
            if start is None:
                start, peak = i, p
            elif (i - last) * dt > max_gap:
                bursts.append((start, last - start + 1, peak))
                start, peak = i, p
            last = i
            peak = max(peak, p)
    if start is not None:
        bursts.append((start, last - start + 1, peak))
    return bursts


def main():
    ap = argparse.ArgumentParser(description="Read FNIRSI FNB-48 measurements")
    ap.add_argument("-d", "--device", default="/dev/hidraw0")
    ap.add_argument("-n", "--duration", type=float, default=1.0,
                    help="read duration in seconds (default 1.0)")
    ap.add_argument("-o", "--csv", metavar="FILE",
                    help="log every sample (100/s) with a timestamp as CSV")
    ap.add_argument("-b", "--burst", type=float, default=0.15,
                    help="burst detection: power threshold in W (default 0.15)")
    ap.add_argument("--max-bursts", type=int, default=10,
                    help="list at most this many bursts (default 10)")
    args = ap.parse_args()

    if not os.path.exists(args.device):
        print(f"ERROR: {args.device} does not exist", file=sys.stderr)
        return 1

    csv_fh = None
    on_sample = None
    if args.csv:
        csv_fh = open(args.csv, "w", buffering=1)
        csv_fh.write("t_s;voltage_V;current_A;power_W;dp_V;dn_V;temp_C;"
                     "energy_Wh;capacity_Ah\n")
        state = {"wh": 0.0, "ah": 0.0, "t": None}

        def on_sample(s, t):
            dt = 0.0 if state["t"] is None else max(0.0, t - state["t"])
            state["t"] = t
            state["wh"] += s["power"] * dt / 3600.0
            state["ah"] += s["current"] * dt / 3600.0
            csv_fh.write(f"{t:.4f};{s['voltage']:.5f};{s['current']:.5f};"
                         f"{s['power']:.5f};{s['dp']:.3f};{s['dn']:.3f};"
                         f"{s['temp']:.1f};{state['wh']:.8f};{state['ah']:.8f}\n")

    try:
        samples, (crc_seen, crc_ok) = read_samples(
            args.device, args.duration, on_sample=on_sample)
    except PermissionError:
        print(f"ERROR: no permission on {args.device} (root required)",
              file=sys.stderr)
        return 1
    except OSError as e:
        print(f"ERROR: cannot access {args.device}: {e}", file=sys.stderr)
        return 1
    finally:
        if csv_fh:
            csv_fh.close()

    if not samples:
        print("No data packets received - is the meter on USB?", file=sys.stderr)
        return 1

    last = samples[-1]
    n = len(samples)
    avg = {k: sum(s[k] for s in samples) / n for k in ("voltage", "current", "power")}
    mn = {k: min(s[k] for s in samples) for k in ("voltage", "current", "power")}
    mx = {k: max(s[k] for s in samples) for k in ("voltage", "current", "power")}

    # Integrate energy/capacity from the samples (fixed 100 S/s rate).
    dt = 1.0 / SAMPLE_RATE
    energy_wh = sum(s["power"] for s in samples) * dt / 3600.0
    capacity_ah = sum(s["current"] for s in samples) * dt / 3600.0

    print(f"Device  : {args.device}  (FNIRSI FNB-48)")
    print(f"Samples : {n}  (last {n} of ~{n/4:.0f} packets)")
    print(f"Voltage : {last['voltage']:.5f} V")
    print(f"Current : {last['current']:.5f} A")
    print(f"Power   : {last['power']:.5f} W   (P = U * I)")
    print(f"D+ / D- : {last['dp']:.3f} V / {last['dn']:.3f} V   "
          f"Temp: {last['temp']:.1f} degC")
    print(f"Power   (W): min {mn['power']:.5f}  max {mx['power']:.5f}  "
          f"avg {avg['power']:.5f}")
    print(f"Voltage (V): min {mn['voltage']:.5f}  max {mx['voltage']:.5f}  "
          f"avg {avg['voltage']:.5f}")
    print(f"Current (A): min {mn['current']:.5f}  max {mx['current']:.5f}  "
          f"avg {avg['current']:.5f}")
    print(f"Energy  : {energy_wh:.6f} Wh   Capacity: {capacity_ah:.6f} Ah")

    bursts = find_bursts([s["power"] for s in samples], threshold=args.burst)
    if bursts:
        gaps = [(bursts[i + 1][0] - bursts[i][0]) * dt
                for i in range(len(bursts) - 1)]
        print(f"Bursts >{args.burst} W: {len(bursts)}", end="")
        if gaps:
            print(f"  spacing avg {sum(gaps)/len(gaps):.3f} s", end="")
        print(f"  peak {max(p for _, _, p in bursts):.5f} W")
        for start, length, peak in bursts[:args.max_bursts]:
            print(f"    +{start*dt:7.3f}s  {length:3d} samples  peak {peak:.5f} W")
    else:
        print(f"Bursts >{args.burst} W: none")

    if crc_seen:
        print(f"CRC     : {'ok' if crc_ok else 'ERROR (at least 1 packet)'}")
    else:
        print("CRC     : not checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
