#!/usr/bin/env python3
"""Charakterisiert die ECHTE Spannungskennlinie beider Cluster per ADC.

Der DT-Wert (min 680 mV) ist eine Deklaration, kein Messwert. Der SARADC hat
einen Kanal je Cluster, der der Rail-Spannung folgt. Dieses Skript

  1. findet fuer JEDEN Cluster den ADC-Kanal, der seinem duty folgt,
  2. vermisst die Kennlinie duty -> gemessene Spannung (Median, wiederholt),
  3. bestimmt den ECHTEN Boden (duty 0) und vergleicht ihn mit dem DT-Wert,
  4. prueft, ob ein duty unter 0 (nicht moeglich) oder eine andere Periode hilft.

Nicht persistent: alles nur im PWM-Register; Reboot stellt Werkswerte her.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv  # noqa: E402

IIO = '/sys/bus/iio/devices/iio:device0'
NCH = 8                      # 0..7 sind die Nutzkanaele (8..12 sind Referenz)
LOG = '/root/adc_curve.log'


def scale():
    return float(open(os.path.join(IIO, 'in_voltage_scale')).read())


def raw(idx):
    return int(open(os.path.join(IIO, 'in_voltage%d_raw' % idx)).read())


def mv(idx, samples=9):
    vals = sorted(raw(idx) for _ in range(samples))
    return vals[len(vals) // 2] * scale()


def log(m):
    line = '%s %s' % (time.strftime('%H:%M:%S'), m)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')
        f.flush()
        os.fsync(f.fileno())


def find_channel(cluster, freq):
    """Welcher ADC-Kanal folgt dem duty dieses Clusters?"""
    uv.pin(cluster, freq)
    period = uv.CLUSTERS[cluster]['period']
    lo, hi = {}, {}
    for duty in (0, period):
        uv.write_duty(cluster, duty)
        time.sleep(0.2)
        for i in range(NCH):
            (lo if duty == 0 else hi)[i] = mv(i, 5)
    deltas = {i: hi[i] - lo[i] for i in range(NCH)}
    best = max(deltas, key=lambda i: abs(deltas[i]))
    return best, deltas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--freq', type=int, default=666666)
    ap.add_argument('--samples', type=int, default=9)
    args = ap.parse_args()

    log('=== ADC-Kennlinie, Referenz %s ===' % {
        'gnd': mv(9, 5), '0.5vdd': mv(11, 5)})

    channels = {}
    for c in ('big', 'little'):
        ch, deltas = find_channel(c, args.freq)
        channels[c] = ch
        log('%s -> ADC ch%d   (delta je Kanal: %s)'
            % (c, ch, ' '.join('%d:%+.0f' % (i, deltas[i]) for i in range(NCH))))

    for c in ('big', 'little'):
        ch = channels[c]
        period = uv.CLUSTERS[c]['period']
        uv.pin(c, args.freq)
        log('--- %s @ %d kHz, ADC ch%d ---' % (c, args.freq, ch))
        rows = []
        for duty in (0, 10, 25, 50, 100, 150, 200, 300, 400, 600, 800, period):
            if duty > period:
                continue
            uv.write_duty(c, duty)
            time.sleep(0.2)
            m = mv(ch, args.samples)
            model = 680000 + duty * 360000 // period
            rows.append((duty, m))
            log('  duty %4d  frac %.4f  Modell %6d mV  gemessen %6.1f mV'
                % (duty, duty / period, model // 1000, m))
        floor = rows[0][1]
        top = rows[-1][1]
        log('  => Boden (duty 0) gemessen %.1f mV; Vollausschlag %.1f mV; '
            'Spanne %.1f mV' % (floor, top, top - floor))
        uv.write_duty(c, uv.uv_to_duty(c, 731000 if c == 'big' else 761000))

    for c in ('big', 'little'):
        uv.revert()
    log('=== zurueckgesetzt ===')


if __name__ == '__main__':
    main()
