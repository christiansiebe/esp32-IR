#!/usr/bin/env python3
"""Soak the big CPU PLL at 2460 MHz (N=205) and verify computation stays correct.

Runs a deterministic integer loop whose result is fixed; any silent bit error
from insufficient voltage changes the hash. Compares the hash at the overclocked
clock against a reference taken at stock 2400 MHz, and measures throughput.

Non-persistent (register poke), restored in finally, every line fsynced.
"""
import mmap
import os
import struct
import time

BASE = 0xff63c000
REG = 0x2f4
MASK = 0x1ff
LOG = '/root/pll_soak.txt'
DURATION = 25.0
ITERS = 2000000

os.sched_setaffinity(0, {4})

_logf = open(LOG, 'w', buffering=1)


def log(msg):
    print(msg, flush=True)
    _logf.write(msg + '\n')
    _logf.flush()
    os.fsync(_logf.fileno())


fd = open('/dev/mem', 'rb+', buffering=0)
m = mmap.mmap(fd.fileno(), 0x1000, offset=BASE)


def rd(o):
    return struct.unpack('<I', m[o:o + 4])[0]


def wr(o, v):
    m[o:o + 4] = struct.pack('<I', v)


def clk(n):
    try:
        return int(open('/sys/kernel/debug/clk/%s/clk_rate' % n).read())
    except Exception:
        return -1


def temp():
    try:
        return int(open('/sys/class/thermal/thermal_zone0/temp').read()) / 1000.0
    except Exception:
        return -1


def work(iters=ITERS):
    h = 0
    for i in range(iters):
        h = (h * 31 + i) & 0xffffffff
    return h


orig = rd(REG)
log('orig N=%d reg=0x%08x temp=%.1fC' % (orig & MASK, orig, temp()))
try:
    wr(REG, (orig & ~MASK) | 200)
    time.sleep(1.0)
    ref = work()
    t0 = time.perf_counter()
    work()
    base = time.perf_counter() - t0
    log('ref N=200 (2400MHz): hash=%d  loop=%.4fs' % (ref, base))

    wr(REG, (orig & ~MASK) | 205)
    time.sleep(1.0)
    t0 = time.perf_counter()
    work()
    oc = time.perf_counter() - t0
    log('oc  N=205 (2460MHz): loop=%.4fs  rel=%+.2f%%' % (oc, 100.0 * (base - oc) / base))

    end = time.time() + DURATION
    n = 0
    err = 0
    t0 = time.perf_counter()
    while time.time() < end:
        if work() != ref:
            err += 1
        n += 1
    el = time.perf_counter() - t0
    log('soak %.0fs: %d runs, %d mismatches, %.2f runs/s, temp=%.1fC' %
        (DURATION, n, err, n / el, temp()))
finally:
    wr(REG, orig)
    log('restored N=%d sys_pll(cached)=%d' % (rd(REG) & MASK, clk('sys_pll')))
    log('DONE')
