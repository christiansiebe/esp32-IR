#!/usr/bin/env python3
"""Margin test: measure the *error rate* at a fixed PLL point, not just pass/fail.

Motivation (see PLL-OC-README.md): stepping N upward until the board hangs gives
a cliff, not a margin. A point that "hangs" and a point that is merely
"marginal" look the same in a binary test. This script instead holds N fixed and
counts silent bit errors over time, with several independent work patterns, so a
marginally-unstable point shows a nonzero error rate *before* it dies.

Why this can still miss a hang: undervoltage causes *timing* failures. A setup/
hold violation either yields a wrong result (this script catches it) or wedges
the core / bus / PLL with no output at all (nothing to catch). This board has no
ECC/EDAC and no RAS error records, so a hang leaves no trace.

Non-persistent: the register is restored in finally. Every line is fsynced so a
watchdog reboot leaves the last state on disk.
"""
import mmap
import os
import struct
import sys
import time

BASE = 0xff63c000
REG = 0x2f4
MASK = 0x1ff
N = int(sys.argv[1]) if len(sys.argv) > 1 else 208
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
LOG = '/root/pll_margin.txt'

os.sched_setaffinity(0, {4})
_f = open(LOG, 'a', buffering=1)


def log(m):
    print(m, flush=True)
    _f.write(m + '\n')
    _f.flush()
    os.fsync(_f.fileno())


fd = open('/dev/mem', 'rb+', buffering=0)
m = mmap.mmap(fd.fileno(), 0x1000, offset=BASE)


def rd(o):
    return struct.unpack('<I', m[o:o + 4])[0]


def wr(o, v):
    m[o:o + 4] = struct.pack('<I', v)


def temp():
    try:
        return int(open('/sys/class/thermal/thermal_zone0/temp').read()) / 1000.0
    except Exception:
        return -1.0


def work_int(it=200000):
    """Integer ALU + branch + shift: the classic hash loop."""
    h = 0
    for i in range(it):
        h = (h * 31 + i) & 0xffffffff
    return h


def work_mix(it=200000):
    """A different path: byte/bit mixing (shift-heavy), to stress other logic."""
    h = 0x12345678
    for i in range(it):
        h ^= (h << 5) & 0xffffffff
        h = (h >> 3) ^ (i * 2654435761 & 0xffffffff)
        h &= 0xffffffff
    return h


PATTERNS = [('int', work_int), ('mix', work_mix)]

orig = rd(REG)
try:
    # Reference at stock 2400 MHz (N=200) for every pattern.
    wr(REG, (orig & ~MASK) | 200)
    time.sleep(1.0)
    ref = {name: fn() for name, fn in PATTERNS}
    log('ref N=200: ' + ' '.join('%s=%d' % (k, v) for k, v in ref.items()))

    wr(REG, (orig & ~MASK) | N)
    time.sleep(1.0)
    log('MARGIN N=%d (%d MHz) ARMED temp=%.1fC reg=0x%08x' % (N, N * 12, temp(), rd(REG)))

    end = time.time() + SECS
    runs = {name: 0 for name, _ in PATTERNS}
    errs = {name: 0 for name, _ in PATTERNS}
    t0 = time.perf_counter()
    while time.time() < end:
        for name, fn in PATTERNS:
            runs[name] += 1
            if fn() != ref[name]:
                errs[name] += 1
    el = time.perf_counter() - t0
    total_runs = sum(runs.values())
    total_errs = sum(errs.values())
    rate = (total_errs / total_runs) if total_runs else 0.0
    log('N=%d %.0fs: runs=%d errs=%d err_rate=%.3e (%.2f run/s) temp=%.1fC %s' %
        (N, SECS, total_runs, total_errs, rate, total_runs / el, temp(),
         'CLEAN' if total_errs == 0 else 'BIT ERRORS'))
    for name, _ in PATTERNS:
        log('  pattern %-4s runs=%d errs=%d' % (name, runs[name], errs[name]))
finally:
    wr(REG, orig)
    log('restored reg=0x%08x N=%d' % (rd(REG), rd(REG) & MASK))
    log('DONE')
