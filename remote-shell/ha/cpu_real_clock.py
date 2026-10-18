#!/usr/bin/env python3
"""cpu-real-clock - make scaling_cur_freq report the REAL CPU clock.

Why: the Amlogic cpufreq-dt driver's scaling_cur_freq is the OPP the driver
*believes* it set. A manual PLL overclock (writing HHI 0xff63c000+0x2f4 for the
big / +0x380 for the little cluster via /dev/mem, see PLL-OC-README.md) is
invisible to the driver, so every reader (cpu-live, cpu-volt-line, htopv, htop
show_cpu_frequency, HA, cpupower) shows a stale 2400 instead of the real 2472.

How: read the live PLL N and the true scaling_cur_freq, then bind-mount a small
tmpfs file carrying the real clock over each policy's scaling_cur_freq. The
per-core paths (cpuN/cpufreq/scaling_cur_freq) are the same inode as the policy
path, so ONE mount per policy covers every reader, and the mount survives CPU
hotplug (verified on the board).

What the real clock is: at the top OPP the driver sets the PLL straight to the
OPP rate (divider 1) - so the PLL *is* the clock, and a manual overclock shows up
here. Below the top OPP the driver holds the PLL high (3000 MHz) and divides it
down, so the PLL is NOT the clock and the driver's value is correct. The two
cases are told apart by the ratio: the PLL sits within a few percent ABOVE the
driver's value only at the top OPP.

The real value is read through a file descriptor opened BEFORE the overlay, so
the daemon never reads back its own output. Every mount is undone on exit.
"""
import argparse
import mmap
import os
import signal
import struct
import subprocess
import sys
import time

HHI_BASE = 0xff63c000
MASK = 0x1ff
PPM = 1.2  # top-OPP detection window: PLL within +20% of the driver's value
RUN = "/run/cpu-real-clock"

# (label, cpufreq policy, HHI register offset of the cluster PLL)
CLUSTERS = (
    ("little", "policy0", 0x380),
    ("big", "policy2", 0x2f4),
)

CPU = os.environ.get("CPU_SYS", "/sys/devices/system/cpu")


def real_khz(pll_n, cached_khz, ppm=PPM):
    """True clock in kHz from the PLL divider count and the driver's value."""
    if not pll_n or cached_khz <= 0:
        return cached_khz
    pll_khz = pll_n * 12000
    if cached_khz <= pll_khz <= cached_khz * ppm:
        return pll_khz
    return cached_khz


def _policy_path(policy):
    return os.path.join(CPU, "cpufreq", policy, "scaling_cur_freq")


def _read_fd(fd):
    try:
        return int(os.pread(fd, 64, 0).decode().strip() or 0)
    except (OSError, ValueError):
        return 0


class Overlay:
    """Bind-mounts a tmpfs file over scaling_cur_freq, updated by tick()."""

    def __init__(self, clusters=CLUSTERS, run=RUN, verbose=False):
        self.clusters = clusters
        self.run = run
        self.verbose = verbose
        self.fds = {}
        self.mounts = []
        self.mem = None

    def _unmount_stale(self):
        for _, policy, _ in self.clusters:
            subprocess.run(["umount", _policy_path(policy)],
                           stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)

    def open(self):
        self._unmount_stale()
        os.makedirs(self.run, exist_ok=True)
        fd = os.open("/dev/mem", os.O_RDWR)
        self.mem = mmap.mmap(fd, 0x1000, offset=HHI_BASE)
        for _, policy, _ in self.clusters:
            # opened before the overlay -> reads the driver's true value
            self.fds[policy] = os.open(_policy_path(policy), os.O_RDONLY)
            src = os.path.join(self.run, policy)
            with open(src, "w") as fh:
                fh.write("0\n")
            if subprocess.run(["mount", "--bind", src, _policy_path(policy)],
                              stderr=subprocess.DEVNULL).returncode == 0:
                self.mounts.append((src, _policy_path(policy)))
            else:
                print("cpu-real-clock: bind-mount failed for %s" % policy,
                      file=sys.stderr)

    def _pll_n(self, off):
        return struct.unpack("<I", self.mem[off:off + 4])[0] & MASK

    def tick(self):
        for label, policy, off in self.clusters:
            cached = _read_fd(self.fds[policy])
            n = self._pll_n(off)
            if cached <= 0:
                # cluster offline: leave it empty so readers show "-", not 0
                content = ""
                khz = None
            else:
                khz = real_khz(n, cached)
                content = "%d\n" % khz
            with open(os.path.join(self.run, policy), "w") as fh:
                fh.write(content)
            if self.verbose:
                print("%-6s cached=%d pll_n=%d -> real=%s" %
                      (label, cached, n, khz), flush=True)

    def close(self):
        for _, path in self.mounts:
            subprocess.run(["umount", path], stderr=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL)
        self.mounts = []
        for fd in self.fds.values():
            os.close(fd)
        self.fds = {}
        if self.mem:
            self.mem.close()
            self.mem = None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-i", "--interval", type=float, default=1.0)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    stop = {"flag": False}

    def _sig(_signum, _frame):
        stop["flag"] = True

    signal.signal(signal.SIGTERM, _sig)
    signal.signal(signal.SIGINT, _sig)

    ov = Overlay(verbose=args.verbose)
    try:
        ov.open()
        if not ov.mounts:
            print("cpu-real-clock: no overlay mounted (pass-through)", file=sys.stderr)
            return 1
        while not stop["flag"]:
            ov.tick()
            if args.once:
                break
            time.sleep(args.interval)
    finally:
        ov.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
