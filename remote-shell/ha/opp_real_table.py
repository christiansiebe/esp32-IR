#!/usr/bin/env python3
"""Reale OPP-Spannungstabelle: nominal (DT) -> real (ADC) fuer den big-Cluster.

Der DT-Wert ist nominal. Der ADC (ch4) misst die reale Rail-Spannung. Dieses
Skript faehrt eine Reihe nominaler Zielwerte an (jeweils in duty umgerechnet,
wie der Regler es tut), misst die reale Spannung und gibt die reale Tabelle
plus die reale Transferfunktion und den realen Boden aus.

Damit laesst sich ein OPP-Wert waehlen, der real die gewuenschte Spannung
ergibt - statt den nominalen DT-Wert zu raten.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv

IIO = '/sys/bus/iio/devices/iio:device0'
S = float(open(os.path.join(IIO, 'in_voltage_scale')).read())
LOG = '/root/opp_real.log'


def mv(idx=4, n=15):
    vals = sorted(int(open(os.path.join(IIO, 'in_voltage%d_raw' % idx)).read())
                  for _ in range(n))
    return vals[len(vals) // 2] * S


def log(m):
    line = '%s %s' % (time.strftime('%H:%M:%S'), m)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')
        f.flush()
        os.fsync(f.fileno())


def main():
    c = 'big'
    uv.pin(c, 666666)
    period = uv.CLUSTERS[c]['period']
    log('=== big: nominal -> real (ADC ch4), period %d ===' % period)
    log('  nominal  duty   real(mV)   Abweichung real-nominal')
    pts = [680000, 700000, 720000, 731000, 750000, 780000, 820000, 900000, 1040000]
    real = {}
    for nom in pts:
        d = uv.uv_to_duty(c, nom)
        uv.write_duty(c, d)
        time.sleep(0.25)
        r = mv()
        real[nom] = r
        log('  %7d  %4d   %7.1f    %+6.1f' % (nom, d, r, r - nom / 1000))

    # Lineare Regression real = a + b*duty
    xs = [uv.uv_to_duty(c, n) for n in pts]
    ys = [real[n] for n in pts]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    a = my - b * mx
    log('  Transferfunktion: real_mV = %.2f + %.5f * duty' % (a, b))
    log('  => realer Boden (duty 0) = %.1f mV' % a)
    log('  => realer Vollausschlag (duty %d) = %.1f mV' % (period, a + b * period))
    log('  => Werks-Tief-Takt nominal 731000 -> real %.1f mV; '
        'Boden real %.1f mV; reale Ersparnis %.1f mV'
        % (real[731000], a, real[731000] - a))

    uv.write_duty(c, uv.uv_to_duty(c, 731000))
    uv.revert()
    log('=== zurueckgesetzt ===')


if __name__ == '__main__':
    main()
