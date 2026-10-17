#!/usr/bin/env python3
"""Stabilitaetsnachweis am ECHTEN Boden - Grundlage fuer eine reale neue Spannung.

Der DT-Wert (680 mV) ist deklariert; der ADC misst fuer den big-Cluster bei
duty 0 real ~721 mV. Dieses Skript faehrt beide Cluster auf die TIEFEN Takte,
setzt den duty auf 0 (Regler-Boden), misst die Spannung und laesst Rechenlast
mit SHA-256-Pruefung laufen. Nur wenn die Hashes stimmen UND die Spannung
stabil bleibt, ist der Boden als neue OPP-Spannung tragfaehig.

Nicht persistent: nur Register, Reboot stellt Werkswerte her.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv
import uvstress as us

IIO = '/sys/bus/iio/devices/iio:device0'
S = float(open(os.path.join(IIO, 'in_voltage_scale')).read())
LOG = '/root/floor_soak.log'


def ch4():
    vals = sorted(int(open(os.path.join(IIO, 'in_voltage4_raw')).read())
                  for _ in range(9))
    return vals[4] * S


def log(m):
    line = '%s %s' % (time.strftime('%H:%M:%S'), m)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')
        f.flush()
        os.fsync(f.fileno())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--freq', type=int, default=666666)
    ap.add_argument('--seconds', type=int, default=60)
    ap.add_argument('--iters', type=int, default=200000)
    ap.add_argument('--workers', type=int, default=4)
    args = ap.parse_args()

    for c in ('big', 'little'):
        uv.pin(c, args.freq)
    ref = us.load(args.iters, args.workers)
    log('=== Boden-Soak %ds @ %d kHz, Referenz-Hashes bei Werksspannung ==='
        % (args.seconds, args.freq))
    log('  big ch4 = %.1f mV (Werksspannung)' % ch4())

    # duty 0 = Regler-Boden (Modell 680 mV, real gemessen ~721 mV)
    for c in ('big', 'little'):
        uv.write_duty(c, 0)
    time.sleep(0.3)
    log('  nach duty=0:  big ch4 = %.1f mV   big duty=%d  little duty=%d'
        % (ch4(), uv.read_duty('big'), uv.read_duty('little')))

    t0 = time.time()
    rounds = 0
    bad_total = 0
    while time.time() - t0 < args.seconds:
        res = us.load(args.iters, args.workers)
        bad = [s for s, h in res.items() if h != ref.get(s)]
        bad_total += len(bad)
        rounds += 1
        if bad:
            log('  RECHENFEHLER in Runde %d: %s' % (rounds, bad))
    log('  %d Runden, %d Rechenfehler, ch4 zuletzt %.1f mV, temp %d C'
        % (rounds, bad_total, ch4(),
           int(open('/sys/class/thermal/thermal_zone0/temp').read()) // 1000))

    # Gegenprobe: Werksspannung wiederherstellen
    for c in ('big', 'little'):
        uv.write_duty(c, uv.uv_to_duty(c, 731000 if c == 'big' else 761000))
    time.sleep(0.2)
    log('  Werksspannung zurueck: big ch4 = %.1f mV' % ch4())
    for c in ('big', 'little'):
        uv.revert()
    log('=== Ergebnis: %s ===' % ('STABIL am Boden' if bad_total == 0
                                  else 'INSTABIL (%d Fehler)' % bad_total))


if __name__ == '__main__':
    main()
