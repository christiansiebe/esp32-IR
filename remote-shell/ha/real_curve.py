#!/usr/bin/env python3
"""Reale Spannungskurve beider Cluster ueber die GEFILTERTEN ADC-Kanaele.

Der rohe ADC-Kanal rauscht (~50 mV). Der Treiber bietet `in_voltageN_mean_raw`
(ein gemitteltes Sample). Damit lassen sich beide Rails sauber vermessen.

Ablauf:
  1. Fuer jeden Cluster den mean-Kanal mit der besten Korrelation zu duty finden.
  2. Reale Kennlinie duty -> mV messen, lineare Regression.
  3. Realen Boden (duty 0) und reale Werks-Tief-Takt-Spannung ausgeben.

Nicht persistent: nur PWM-Register; Reboot stellt Werkswerte her.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv

IIO = '/sys/bus/iio/devices/iio:device0'
S = float(open(os.path.join(IIO, 'in_voltage_scale')).read())
LOG = '/root/real_curve.log'


def mean_mv(i, n=7):
    vals = sorted(int(open(os.path.join(IIO, 'in_voltage%d_mean_raw' % i)).read())
                  for _ in range(n))
    return vals[len(vals) // 2] * S


def corr(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    dx = sum((x - mx) ** 2 for x in xs) ** 0.5
    dy = sum((y - my) ** 2 for y in ys) ** 0.5
    return num / (dx * dy) if dx and dy else 0.0


def log(m):
    line = '%s %s' % (time.strftime('%H:%M:%S'), m)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')
        f.flush()
        os.fsync(f.fileno())


def find_channel(c):
    uv.pin(c, 666666)
    period = uv.CLUSTERS[c]['period']
    probes = [0, period // 4, period // 2, 3 * period // 4, period]
    ser = {i: [] for i in range(8)}
    for d in probes:
        uv.write_duty(c, d)
        time.sleep(0.25)
        for i in range(8):
            ser[i].append(mean_mv(i, 5))
    best, bestr = None, 0.0
    for i in range(8):
        r = corr(probes, ser[i])
        if abs(r) > abs(bestr):
            best, bestr = i, r
    return best, bestr, {i: corr(probes, ser[i]) for i in range(8)}


def main():
    log('=== reale Spannungskurve ueber mean-Kanaele ===')
    chans = {}
    for c in ('big', 'little'):
        ch, r, allr = find_channel(c)
        chans[c] = ch
        log('%s -> mean ch%d (r=%+.3f)  alle: %s'
            % (c, ch, r, ' '.join('%d:%+.2f' % (i, allr[i]) for i in range(8))))

    for c in ('big', 'little'):
        ch = chans[c]
        period = uv.CLUSTERS[c]['period']
        uv.pin(c, 666666)
        pts = [0, period // 8, period // 4, period // 2, 3 * period // 4, period]
        log('--- %s @ 666666 kHz, mean ch%d ---' % (c, ch))
        xs, ys = [], []
        for d in pts:
            uv.write_duty(c, d)
            time.sleep(0.25)
            m = mean_mv(ch)
            xs.append(d)
            ys.append(m)
            log('  duty %4d  frac %.3f  Modell %6.0f mV  real %7.1f mV'
                % (d, d / period, (680000 + d * 360000 / period) / 1000, m))
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
        a = my - b * mx
        stock = uv.uv_to_duty(c, 731000 if c == 'big' else 761000)
        log('  Transfer: real_mV = %.1f + %.5f * duty' % (a, b))
        log('  realer Boden (duty 0) = %.1f mV | bei Werks-Tief-Takt (duty %d) = %.1f mV'
            % (a, stock, a + b * stock))
        uv.write_duty(c, stock)

    for c in ('big', 'little'):
        uv.revert()
    log('=== zurueckgesetzt ===')


if __name__ == '__main__':
    main()
