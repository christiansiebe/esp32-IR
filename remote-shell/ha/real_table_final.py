#!/usr/bin/env python3
"""Finale reale Spannungstabelle beider Cluster.

big:  nominal (DT) -> real (ADC mean ch4, gefiltert) + lineare Regression.
little: Regler-Zustand + alle ADC-Kanaele bei duty 0 vs. Vollausschlag, um zu
        klaeren, ob VDDCPU_B ueberhaupt messbar ist.

Nicht persistent: nur PWM-Register; Reboot stellt Werkswerte her.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv

IIO = '/sys/bus/iio/devices/iio:device0'
S = float(open(os.path.join(IIO, 'in_voltage_scale')).read())
LOG = '/root/real_table_final.log'


def mean_mv(i, n=9):
    v = sorted(int(open(os.path.join(IIO, 'in_voltage%d_mean_raw' % i)).read())
               for _ in range(n))
    return v[len(v) // 2] * S


def log(m):
    line = '%s %s' % (time.strftime('%H:%M:%S'), m)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')
        f.flush()
        os.fsync(f.fileno())


def main():
    log('=== reale Spannungstabelle ===')

    c = 'big'
    uv.pin(c, 666666)
    period = uv.CLUSTERS[c]['period']
    log('--- big (ADC mean ch4) ---')
    log('  nominal   duty   real(mV)   real-nominal')
    pts = [680000, 700000, 731000, 760000, 800000, 900000, 1040000]
    xs, ys = [], []
    for nom in pts:
        d = uv.uv_to_duty(c, nom)
        uv.write_duty(c, d)
        time.sleep(0.25)
        r = mean_mv(4)
        xs.append(d)
        ys.append(r)
        log('  %7d  %4d  %7.1f   %+6.1f' % (nom, d, r, r - nom / 1000))
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    a = my - b * mx
    d_stock = uv.uv_to_duty(c, 731000)
    log('  Transfer: real_mV = %.1f + %.5f*duty' % (a, b))
    log('  realer Boden (duty 0)        = %.1f mV' % a)
    log('  Werks-Tief-Takt (duty %3d)    = %.1f mV' % (d_stock, a + b * d_stock))
    log('  reale Ersparnis Werks->Boden = %.1f mV' % (a + b * d_stock - a))

    c = 'little'
    uv.pin(c, 666666)
    period = uv.CLUSTERS[c]['period']
    log('--- little: Kanal-Suche (alle mean-Kanaele) ---')
    for d in (0, period):
        uv.write_duty(c, d)
        time.sleep(0.3)
        log('  duty %4d: %s' % (d, ' '.join('ch%d=%.0f' % (i, mean_mv(i, 9))
                                          for i in range(8))))
    log('  regulator.8 (VDDCPU_B) now = %s uV'
        % open('/sys/class/regulator/regulator.8/microvolts').read().strip())

    for c in ('big', 'little'):
        uv.revert()
    log('=== zurueckgesetzt ===')


if __name__ == '__main__':
    main()
