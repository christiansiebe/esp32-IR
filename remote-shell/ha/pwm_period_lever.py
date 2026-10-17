#!/usr/bin/env python3
"""Ist die PWM-Periode ein Spannungshebel bei FESTEM duty?

Frueher wurde nur bei duty 0 gemessen - dort ist der Bruch duty/period immer 0,
also kann die Periode nichts aendern. Hier wird bei festem, von Null
verschiedenem duty gemessen: aendert die Periode die reale Spannung? Das
entscheidet, ob der Bruch duty/period der einzige Knopf ist (dann gibt es keine
zusaetzlichen Zwischenwerte) oder ob die Periode ein zweiter Hebel ist.

Nicht persistent: nur Register.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv
import struct

IIO = '/sys/bus/iio/devices/iio:device0'
S = float(open(os.path.join(IIO, 'in_voltage_scale')).read())


def mv(idx=4, n=11):
    vals = sorted(int(open(os.path.join(IIO, 'in_voltage%d_raw' % idx)).read())
                  for _ in range(n))
    return vals[len(vals) // 2] * S


def set_period_duty(c, period, duty):
    duty = max(0, min(period, int(duty)))
    uv.write_reg(c, ((period - duty) << 16) | duty)


def main():
    c = 'big'
    uv.pin(c, 666666)
    print('=== %s: Periode bei festem duty, ADC ch4 ===' % c)
    for duty in (200, 400, 600):
        print('--- duty = %d ---' % duty)
        for period in (999, 1998, 3996, 7992, 15984):
            set_period_duty(c, period, duty)
            time.sleep(0.25)
            frac = duty / period
            model = 680000 + duty * 360000 / period
            print('  period %5d  frac %.4f  Modell %6.0f mV  gemessen %6.1f mV'
                  % (period, frac, model / 1000, mv()))
    # Werkszustand: big period 999, duty fuer 731 mV
    set_period_duty(c, 999, uv.uv_to_duty(c, 731000))
    uv.revert()
    print('=== zurueckgesetzt ===')


if __name__ == '__main__':
    main()
