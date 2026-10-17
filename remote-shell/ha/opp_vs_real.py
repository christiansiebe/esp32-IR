#!/usr/bin/env python3
"""Regler-Meldung vs. reale Spannung - und Kopplung der beiden Rails.

Der Regler meldet eine Spannung, die aus dem DT-Modell berechnet ist (nominal).
Der ADC misst die reale Rail. Dieses Skript vergleicht beide ueber alle Takte
und prueft, ob VDDCPU_A und VDDCPU_B dieselbe physische Rail sind.

Ablauf ueber das cpufreq-Framework (der Treiber programmiert den Regler):
  big   (policy2): je Takt  regulator.2/microvolts  vs.  ADC ch4 (mean)
  little(policy0): je Takt  regulator.8/microvolts  vs.  alle ADC-Kanaele
  Kopplung: big fix, little duty variieren -> bewegt sich ch4?

Nicht persistent: Governor am Ende zurueck auf schedutil.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv

IIO = '/sys/bus/iio/devices/iio:device0'
S = float(open(os.path.join(IIO, 'in_voltage_scale')).read())
LOG = '/root/opp_vs_real.log'


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


def freqs(pol):
    return [int(f) for f in open('/sys/devices/system/cpu/cpufreq/%s/'
                                 'scaling_available_frequencies' % pol).read().split()]


def set_freq(pol, f):
    open('/sys/devices/system/cpu/cpufreq/%s/scaling_governor' % pol, 'w').write('userspace')
    open('/sys/devices/system/cpu/cpufreq/%s/scaling_setspeed' % pol, 'w').write(str(f))
    time.sleep(0.15)


def reg_uv(n):
    return int(open('/sys/class/regulator/regulator.%s/microvolts' % n).read())


def main():
    orig = {p: open('/sys/devices/system/cpu/cpufreq/%s/scaling_governor' % p).read().strip()
            for p in ('policy0', 'policy2')}
    try:
        log('=== big: Regler-Meldung vs. ADC ch4 (real) ===')
        log('   freq(kHz)   Regler(mV)   ADC ch4(mV)   real-nominal')
        for f in freqs('policy2'):
            set_freq('policy2', f)
            r = reg_uv(2)
            a = mean_mv(4)
            log('   %8d   %8.1f   %9.1f   %+8.1f' % (f, r / 1000, a, a - r / 1000))

        log('=== little: Regler-Meldung vs. ADC-Kanaele ===')
        for f in freqs('policy0'):
            set_freq('policy0', f)
            r = reg_uv(8)
            chans = ' '.join('ch%d=%.0f' % (i, mean_mv(i, 5)) for i in (2, 3, 4, 5))
            log('   %8d   Regler %8.1f mV   %s' % (f, r / 1000, chans))

        log('=== Kopplungstest: big fix, little duty variieren, ch4 beobachten ===')
        # WICHTIG: beide Cluster zuerst auf einen TIEFEN Takt festnageln. Wird
        # der little-duty auf 0 gesetzt, waehrend little noch auf einem hohen
        # Takt laeuft, ist das ein massives Undervolt -> Board-Hang (passiert
        # beim ersten Lauf, Watchdog-Reboot). Darum erst freq senken.
        set_freq('policy2', 666666)
        set_freq('policy0', 666666)
        uv.pin('big', 666666)
        uv.pin('little', 666666)
        uv.write_duty('big', uv.uv_to_duty('big', 731000))
        uv.write_duty('little', uv.uv_to_duty('little', 761000))
        time.sleep(0.2)
        base = mean_mv(4)
        log('   big fix, ch4 = %.1f mV' % base)
        for d in (0, 166, 333):
            uv.write_duty('little', d)
            time.sleep(0.25)
            log('   little duty %3d -> ch4 = %.1f mV (delta %+.1f)'
                % (d, mean_mv(4), mean_mv(4) - base))
    finally:
        for p, g in orig.items():
            open('/sys/devices/system/cpu/cpufreq/%s/scaling_governor' % p, 'w').write(g)
        for c in ('big', 'little'):
            uv.revert()
        log('=== Governor zurueck: %s ===' % orig)


if __name__ == '__main__':
    main()
