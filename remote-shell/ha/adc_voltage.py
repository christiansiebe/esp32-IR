#!/usr/bin/env python3
"""Misst die CPU-Spannung ueber den SARADC (Odroid N2+).

Es gibt keinen Spannungsregler-Sensor, aber der SARADC des G12A/B hat einen
Kanal, der der big-Cluster-Spannung VDDCPU_A monoton folgt (ch4). Damit laesst
sich die Spannung MESSEN statt sie aus dem Device Tree zu glauben.

Der ADC ist selbstkalibrierend: die Kanaele 9..12 tragen gnd, 0.25vdd, 0.5vdd,
0.75vdd, also eine eingebaute Referenz (1.8 V). `scale` ist 0.439453125 mV/LSB.

Wichtig: der gemessene Boden (duty 0) liegt bei ~730 mV, NICHT bei den 680 mV
aus dem Device Tree - die DT-Angabe ist konservativ.

Nutzung:
    python3 adc_voltage.py            # nur messen
    python3 adc_voltage.py --sweep    # Kennlinie duty -> Spannung zeigen
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv  # noqa: E402

IIO = '/sys/bus/iio/devices/iio:device0'
CH_VDOCPU_A = 4          # folgt VDDCPU_A (big); monoton, per Sweep bestaetigt
REF = {9: 'gnd', 10: '0.25vdd', 11: '0.5vdd', 12: '0.75vdd'}


def scale():
    return float(open(os.path.join(IIO, 'in_voltage_scale')).read())


def raw(idx):
    return int(open(os.path.join(IIO, 'in_voltage%d_raw' % idx)).read())


def measure_mv(idx=CH_VDOCPU_A, samples=7):
    """Median ueber mehrere Lesungen gegen Ausreisser."""
    vals = sorted(raw(idx) for _ in range(samples))
    return vals[len(vals) // 2] * scale()


def self_test():
    """Referenzkanaele pruefen - bei 1.8 V muss 0.5vdd ~900 mV zeigen."""
    s = scale()
    out = {}
    for idx, name in REF.items():
        out[name] = round(raw(idx) * s, 1)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sweep', action='store_true')
    ap.add_argument('--cluster', default='big', choices=['big', 'little'])
    ap.add_argument('--freq', type=int, default=15625)
    args = ap.parse_args()

    print('ADC-Selbsttest: %s' % self_test())
    print('VDDCPU_A (ch%d): %.1f mV' % (CH_VDOCPU_A, measure_mv()))

    if not args.sweep:
        return
    c = args.cluster
    uv.pin(c, args.freq)
    reg0 = uv.read_reg(c)
    period = uv.CLUSTERS[c]['period']
    print('Kennlinie %s @ %d kHz (Periode %d):' % (c, args.freq, period))
    for duty in (0, 100, 250, 500, 750, period):
        uv.write_duty(c, duty)
        time.sleep(0.15)
        print('  duty %4d  frac %.3f  gemessen %6.1f mV'
              % (duty, duty / period, measure_mv()))
    uv.write_reg(c, reg0)
    uv.revert()


if __name__ == '__main__':
    main()
