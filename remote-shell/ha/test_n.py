#!/usr/bin/env python3
"""Test one big-CPU PLL point (N) for stability. Usage: test_n.py <N> [secs]

Sets sys_pll = N * 12 MHz, runs a deterministic hash-checked loop, restores.
A hang here is caught by the 60 s hardware watchdog; the last fsynced line in
/root/pll_n.txt then marks the failing N. Non-persistent (register poke).
"""
import mmap
import os
import struct
import sys
import time

BASE = 0xff63c000
REG = 0x2f4
MASK = 0x1ff
N = int(sys.argv[1])
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
LOG = '/root/pll_n.txt'

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
    return int(open('/sys/class/thermal/thermal_zone0/temp').read()) / 1000.0


def work(it=1000000):
    h = 0
    for i in range(it):
        h = (h * 31 + i) & 0xffffffff
    return h


orig = rd(REG)
try:
    wr(REG, (orig & ~MASK) | 200)
    time.sleep(1.0)
    ref = work()

    log('TRY N=%d (%d MHz) START temp=%.1fC' % (N, N * 12, temp()))
    wr(REG, (orig & ~MASK) | N)
    time.sleep(1.0)
    t0 = time.perf_counter()
    h = work()
    t = time.perf_counter() - t0

    end = time.time() + SECS
    n = 0
    err = 0
    while time.time() < end:
        if work() != ref:
            err += 1
        n += 1
    log('N=%d (%d MHz) %s hash_ok=%s loop=%.4fs runs=%d errs=%d temp=%.1fC' %
        (N, N * 12, 'PASS' if err == 0 else 'FAIL', h == ref, t, n, err, temp()))
finally:
    wr(REG, orig)
    log('N=%d restored' % N)
