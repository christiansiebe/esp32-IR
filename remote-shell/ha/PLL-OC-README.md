# Odroid N2+ CPU PLL overclock probe (big cluster)

Measured on the board `odroidn2` (Hardkernel ODROID-N2Plus, Amlogic S922X,
big = 4x Cortex-A73). Tooling: `pll_oc_probe.py`, `test_n.py`, `soak_oc.py`,
`sweep_oc.py`, `regdump.py`.

## Register

The big-cluster CPU PLL lives in the HHI block `0xff63c000`, register `+0x2f4`
(little cluster: `+0x380`). Bits [8:0] hold the integer multiplier **N**.
Source is the fixed 24 MHz xtal, the DCO runs at `N * 24 MHz` behind a fixed
`/2` post-divider:

```
sys_pll = N * 12 MHz          (big)
little  = N * 12 MHz          (checked the same way)
```

Measured correlation (reg 0x2f4 vs. `sys_pll` in debugfs):

| freq | N (bits [8:0]) | reg 0x2f4 |
|------|----------------|-----------|
| 2400 MHz (stock max) | 200 | 0xd80104c8 |
| 2208 MHz | 184 | 0xd80104b8 |
| 2016 MHz | 168 | 0xd80104a8 |
| 1800 MHz | 150 | 0xd8010496 |

**2450 MHz is not representable**: N would be 204.17. The nearest points are
**2448 (N=204)** and **2460 (N=205)**.

## Why cpufreq reports 2400 even when the PLL is poked

`clk_rate` in debugfs and `scaling_cur_freq` both come from the kernel's
*cached* rate. They keep reporting 2400 after a raw register write. The only
honest check is **wall-clock time for fixed work**, timed with `perf_counter`
(arch timer, independent of the CPU PLL): a faster CPU finishes the same loop
sooner, and throughput scales linearly with the clock.

Measured (2M-iteration integer loop, pinned to a big core):

| N | target | loop | relative |
|---|--------|------|----------|
| 200 | 2400 MHz | 0.4379 s | base |
| 204 | 2448 MHz | 0.4289 s | +2.06 % (expected +2.00 %) |
| 205 | 2460 MHz | 0.4272 s | +2.43 % (expected +2.50 %) |

The speed-up matches the frequency ratio, so the hardware really followed the
register.

## Stability

`test_n.py` sets N, runs a deterministic hash-checked loop, and compares the
hash against a reference taken at stock 2400 MHz. Any silent bit error from
insufficient voltage changes the hash.

| N | MHz | result |
|---|-----|--------|
| 204 | 2448 | PASS |
| 205 | 2460 | PASS, 25 s soak, 59 runs, 0 errors, 34.8 -> 38.6 C |
| 206 | 2472 | PASS |
| 207 | 2484 | PASS |
| 208 | 2496 | PASS, 120 s soak, 570 runs, 0 errors |
| 209 | 2508 | PASS once (10 s), **hang on the 90 s soak** |
| 210 | 2520 | marginal: hung once, passed 2x afterwards |
| 211 | 2532 | **hang** (watchdog reboot) |

### A pass is a statement about the test length, not about margin

`margin_oc.py` was written to find out whether a point that is *about to* fail
shows bit errors first, so a margin could be measured instead of only a cliff.
It holds N fixed, runs two independent work patterns, and counts the error
*rate* over time rather than a single pass/fail hash.

| N | MHz | margin run | result |
|---|-----|-----------|--------|
| 204 | 2448 | 20 s, 244 runs | CLEAN, err_rate 0, 32.3 -> 35.8 C |
| 206 | 2472 | 15 s, 182 runs | CLEAN, err_rate 0, 31.7 -> 42.7 C |
| 206 | 2472 | **1800 s (30 min), 22830 runs** | **CLEAN, err_rate 0, 31.8 -> 40.9 C** |
| 208 | 2496 | - | **hard reset, no bit error logged first** |

The result overturns the earlier reading of this table. **N=208 is not a safe
ceiling**: it had passed a 120 s soak with 570 runs and 0 errors, and the very
next run at the same N hard-reset the board. The margin test produced **no bit
error before the hang** - the reset was abrupt.

### The kernel rewrites the PLL register under you

A raw poke is only meaningful if nothing else touches the register. It does:
`reg 0x2f4` was observed cycling `N=168 -> 200 -> 168 -> 250 -> 200` about twice
a second with no test running. The **schedutil governor** re-programs the CPU PLL
on every frequency transition, so it silently overwrites the value the test just
wrote - the measured point is then not the one that was set.

Set the governor to `performance` first (`scaling_governor` on `policy0` and
`policy2`), which pins the frequency and stops the writes; `reg 0x2f4` then held
a stable `0xd80104c8` (N=200). Every OC number taken before that fix is suspect.
Also bring the big cores online first: the `cpu-hotplug` daemon leaves only
`cpu0` up when idle, and `margin_oc.py` pins to CPU4.

### Why bit-error detection does not work here

The failure mode of undervolting is a *timing* violation. A setup/hold violation
either produces a wrong result - which a hash catches - or it wedges the
core/bus/PLL with no output at all, which nothing catches. On this board the
second path dominates:

- No ECC/EDAC: `/sys/devices/system/edac` does not exist, so memory errors are
  invisible.
- No RAS records and the ARM SError counter stays 0.
- `pstore`/ramoops is empty after a hard reset (see `RAMOOPS-README.md`), so
  a hang leaves no trace at all.

**Consequence: a binary pass/fail test can only ever report "it survived the
test", never a distance to the edge.** There is no gradient to measure on this
silicon at these clocks - only a cliff. The only honest statements are "held for
X seconds" and "hung". Do not describe any N here as "stable"; describe the
durations that were actually survived.

Practical ceiling observed: **2472 MHz (N=206) held a clean 30-minute run**
(22830 runs, 0 errors, 40.9 C peak) and 2448 MHz (N=204) held a clean 20 s run;
2496 MHz is not reproducible, and 2508 MHz and above hang. At 2460 MHz the board
stays cool (<= 39 C under load).

Note what the 30-minute run does and does not say: it says 2472 MHz survived
1800 s with no detected error. It does **not** say the point has margin - N=208
survived 120 s and then hard-reset on the next attempt, so a longer soak is
still only a longer statement about duration, not a proof of safety. A soak that
ends in a hang and a soak that ends clean look identical up to the moment of the
hang.

## Important caveats

- **Voltage is already at the ceiling.** At stock 2400 MHz the PWM regulator
  sits at 100 % duty (999/999 big, 333/333 little) = 1040 mV (ADC measured
  1027 mV). Every point above 2400 MHz therefore runs *undervolted* relative to
  the OPP curve - there is no headroom to raise the voltage to make a higher
  clock safer.
- **Non-persistent by design.** These tools poke the register directly; a
  reboot reloads the driver's OPP table and returns to stock 2400 MHz. No DTB
  or OPP change is involved. Persisting would need a new OPP entry via
  `gen_lowfreq_dts.py` (which currently only adds points *below* 1 GHz) and a
  boot-loop risk if the point is unstable.
- **A raw poke is not the driver's safe sequence.** Writing the PLL while the
  CPU is clocked from it can glitch the clock; the driver muxes to a
  glitch-free parent, reprograms, then muxes back. A hang from a raw poke is
  therefore not proof that the silicon fails at that clock.
- The reference box here is **Armbian**, not Hardkernel's Ubuntu image, so
  there is no `/media/boot/config.ini`. The clock table comes from the DTB.
  Hardkernel's own table marks 2304/2400 MHz (big) and 2016 MHz (little) as
  *overclock*; Armbian's `meson-g12b-odroid-n2-plus.dtb` already ships those
  OPP entries, so the board runs at 2400/2016 MHz out of the box.
