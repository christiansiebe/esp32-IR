#!/usr/bin/env python3
"""Non-persistent overclock probe of the big CPU PLL (HHI 0xff63c000 + 0x2f4).

The OPP table stops at 2400 MHz, so cpufreq clamps there. This pokes the raw
PLL multiplier N (bits [8:0]) directly. Clock source is the 24 MHz xtal, DCO
runs at N * 24 MHz behind a fixed /2 post-divider:

    sys_pll = N * 12 MHz

2450 MHz is NOT representable (N would be 204.17); the nearest points are
2448 (N=204) and 2460 (N=205). Stock 2400 MHz is N=200.

Why a throughput test: clk_rate in debugfs and scaling_cur_freq both come from
the kernel's *cached* rate, so they keep reporting 2400 even if the hardware PLL
changed. The only honest check is wall-clock time for fixed work, measured with
perf_counter (arch timer, independent of the CPU PLL): a faster CPU finishes the
same loop sooner. Throughput scales linearly with the CPU clock.

Nothing is persisted: a reboot reloads the driver's OPP table. A hang is caught
by the 60 s hardware watchdog. The register is restored in a finally block and
every line is fsynced, because / is mounted commit=120 and a hard reset would
otherwise lose the result.
"""
import mmap
import os
import struct
import time

BASE = 0xff63c000
REG = 0x2f4          # sys_pll (big cluster), multiplier in bits [8:0]
MASK = 0x1ff
LOG = '/root/pll_out.txt'

try:
    os.sched_setaffinity(0, {4})   # pin to a big (A73) core
except Exception as exc:
    print('affinity: %s' % exc)

_logf = open(LOG, 'w', buffering=1)


def log(msg):
    print(msg, flush=True)
    _logf.write(msg + '\n')
    _logf.flush()
    os.fsync(_logf.fileno())


fd = open('/dev/mem', 'rb+', buffering=0)
m = mmap.mmap(fd.fileno(), 0x1000, offset=BASE)


def rd(off):
    return struct.unpack('<I', m[off:off + 4])[0]


def wr(off, val):
    m[off:off + 4] = struct.pack('<I', val)


def clk(name):
    try:
        return int(open('/sys/kernel/debug/clk/%s/clk_rate' % name).read())
    except Exception:
        return -1


def workload(iters=2000000):
    h = 0
    for i in range(iters):
        h = (h * 31 + i) & 0xffffffff
    return h


def best_of(runs=3):
    best = 1e9
    for _ in range(runs):
        t0 = time.perf_counter()
        workload()
        t1 = time.perf_counter()
        best = min(best, t1 - t0)
    return best


orig = rd(REG)
log('orig 0x%03x = 0x%08x  N=%d  sys_pll(cached)=%d' %
    (REG, orig, orig & MASK, clk('sys_pll')))
log('cpu=%d  governor=%s' % (
    min(os.sched_getaffinity(0)),
    open('/sys/devices/system/cpu/cpufreq/policy2/scaling_governor').read().strip()))

try:
    base = None
    for n in (200, 204, 205, 200):
        wr(REG, (orig & ~MASK) | n)
        time.sleep(1.0)
        t = best_of()
        if base is None:
            base = t
        log('N=%3d want=%4dMHz reg=0x%08x  loop=%.4fs  %5.1f Mops/s  rel=%+.2f%%' %
            (n, n * 12, rd(REG), t, 2.0 / t, 100.0 * (base - t) / base))
finally:
    wr(REG, orig)
    log('restored 0x%03x = 0x%08x  sys_pll(cached)=%d' % (REG, rd(REG), clk('sys_pll')))
    log('DONE')
