#!/usr/bin/env python3
"""Host test for cpu_real_clock.real_khz (pure logic, no board).

The top-OPP detection is the one piece of judgement in the tool: the live PLL
equals the clock only at the top OPP (divider 1); below it the driver holds the
PLL high and divides it down, so the driver's scaling_cur_freq is already right.
These cases pin that rule down without touching /dev/mem or sysfs.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "cpu_real_clock", os.path.join(HERE, "cpu_real_clock.py"))
crc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(crc)

checks = 0
fails = 0


def eq(got, want, what):
    global checks, fails
    checks += 1
    if got != want:
        fails += 1
        print("  FAIL %s: got %r want %r" % (what, got, want))


# Big cluster: stock 2400 (N=200) and the 2472 overclock (N=206).
eq(crc.real_khz(200, 2400000), 2400000, "stock big: PLL == clock")
eq(crc.real_khz(206, 2400000), 2472000, "OC big: real 2472 reported")

# Little cluster stock 2016 (N=168) and its overclock points.
eq(crc.real_khz(168, 2016000), 2016000, "stock little")
eq(crc.real_khz(174, 2016000), 2088000, "OC little 2088")

# Below the top OPP the driver divides the (still high) PLL: keep the driver's
# value, do not invent one. Little at 250 MHz, PLL left at N=250 (3000 MHz).
eq(crc.real_khz(250, 250000), 250000, "low OPP little: driver value kept")
eq(crc.real_khz(233, 500000), 500000, "low OPP: driver value kept")
eq(crc.real_khz(200, 666666), 666666, "mid OPP: driver value kept")

# Guard rails.
eq(crc.real_khz(0, 2400000), 2400000, "no PLL reading -> pass through")
eq(crc.real_khz(200, 0), 0, "offline cluster stays offline")

# A cluster genuinely below its PLL (a big overclock far above the OPP) must not
# be clamped: N=250 (3000 MHz) is >20% above 2400, so it is NOT treated as the
# top OPP -> the driver value is kept (conservative).
eq(crc.real_khz(250, 2400000), 2400000, "implausible jump rejected")

print("test_cpu_real_clock: %d checks, %d failures" % (checks, fails))
sys.exit(1 if fails else 0)
