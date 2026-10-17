#!/usr/bin/env python3
"""Diskriminator: senkt eine laengere PWM-Periode die effektive Spannung?

Modell (am Board validiert):  V = 680 mV + (duty/period) * 360 mV
Bei duty=0 ist V = 680 mV, unabhaengig von der Periode. Die Frage ist, ob sich
die Spannung ueber die PERIODE senken laesst, wenn man den duty-Wert FIX haelt:

  duty = 222, period 999   -> V = 680 + 222/999*360   = 760 mV  (sicher)
  duty = 222, period 3996  -> V = 680 + 222/3996*360  = 700 mV  (Grenze!)
  duty = 222, period 7992  -> V = 680 + 222/7992*360  = 690 mV  (haengt?)

Getestet bei big 1.908 GHz: dort ist die Silizium-Grenze bekannt (~700 mV
haengt, ~720 stabil). Haengt das Board, wenn die Periode den Modellwert unter
700 drueckt, ist die Periode ein ECHTER Spannungshebel. Bleibt es stabil, dann
aendert die Periode die Spannung nicht (nur die Schaltfrequenz).

Alles im Register -> Reboot stellt den Werkszustand her.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv
import uvstress as us

LOG = '/root/pwm_shift_test.log'


def log(msg):
    line = '%s %s' % (time.strftime('%H:%M:%S'), msg)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')
        f.flush()
        os.fsync(f.fileno())


def model_uv(duty, period):
    return 680000 + duty * 360000 // period


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--freq', type=int, default=1908000)
    ap.add_argument('--duty', type=int, default=222)
    ap.add_argument('--iters', type=int, default=150000)
    ap.add_argument('--workers', type=int, default=4)
    ap.add_argument('--periods', default='999,1998,2997,3996,5994,7992')
    args = ap.parse_args()

    c = 'big'
    uv.pin(c, args.freq)
    reg0 = uv.read_reg(c)
    log('=== Perioden-Spannungstest: big @ %d kHz, duty=%d fest ==='
        % (args.freq, args.duty))
    log('  Register vorher: 0x%08x' % reg0)

    ref = us.load(args.iters, args.workers)
    log('  Referenz-Hash: %s' % ref)

    for period in [int(p) for p in args.periods.split(',')]:
        if args.duty > period:
            continue
        uv.write_reg(c, ((period - args.duty) << 16) | args.duty)
        time.sleep(0.1)
        got = uv.read_reg(c)
        res = us.load(args.iters, args.workers)
        bad = [s for s, h in res.items() if h != ref.get(s)]
        log('  P=%-6d duty=%-4d frac=%.4f  Modell-V=%4d mV  reg=0x%08x  %s'
            % (period, args.duty, args.duty / period,
               model_uv(args.duty, period) // 1000, got,
               'OK' if not bad else 'RECHENFEHLER=%d' % len(bad)))

    uv.write_reg(c, reg0)
    log('Register wiederhergestellt: 0x%08x' % uv.read_reg(c))
    uv.revert()
    log('=== fertig ===')


if __name__ == '__main__':
    main()
