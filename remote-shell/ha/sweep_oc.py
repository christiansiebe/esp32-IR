#!/usr/bin/env python3
"""Step the big CPU PLL upward until it fails, to find the stable limit.

Voltage is already at the regulator ceiling (1040 mV) at stock 2400 MHz, so
every step above that runs undervolted relative to the OPP curve. Each step is
logged and fsynced *before* the next one, so if the board hangs and the 60 s
hardware watchdog reboots it, the last logged N is the failing point.

Non-persistent: the register is restored in finally, and a watchdog reboot
reloads the driver's OPP table anyway.
"""
import mmap
import os
import struct
import time

BASE = 0xff63c000
REG = 0x2f4
MASK = 0x1ff
LOG = '/root/pll_sweep.txt'
ITERS = 1000000

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


def work(iters=ITERS):
    h = 0
    for i in range(iters):
        h = (h * 31 + i) & 0xffffffff
    return h


orig = rd(REG)
wr(REG, (orig & ~MASK) | 200)
time.sleep(1.0)
ref = work()
log('ref N=200 (2400MHz) hash=%d' % ref)

try:
    for n in (205, 210, 215, 220, 225, 230, 235, 240, 245, 250):
        log('--- trying N=%d (%d MHz) ---' % (n, n * 12))
        wr(REG, (orig & ~MASK) | n)
        time.sleep(1.0)
        t0 = time.perf_counter()
        h = work()
        t = time.perf_counter() - t0
        log('N=%d reg=0x%08x hash=%d %s loop=%.4fs' %
            (n, rd(REG), h, 'OK' if h == ref else 'MISMATCH', t))
        if h != ref:
            log('stopping: mismatch at N=%d' % n)
            break
finally:
    wr(REG, orig)
    log('restored N=%d' % (rd(REG) & MASK))
    log('DONE')
