#!/usr/bin/env python3
"""Host-Tests fuer gen_lowfreq_dts.py (reine Erzeugung, kein Board, kein dtc).

Der Generator ist die einzige Stelle, an der eine Spannung dauerhaft in die
OPP-Tabelle geschrieben wird. Ein zu niedriger Wert hier kann das Board in
einen Boot-Loop schicken, deshalb wird die Spannungslogik geprueft: Werkswert
ohne Argument, expliziter Wert je Cluster, Ablehnung unter dem Regler-Boden und
die Rueckwaerts-Kompatibilitaet des Standardausgabe.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, 'gen_lowfreq_dts.py')

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def gen(args, out):
    r = subprocess.run([sys.executable, GEN, '--out', out] + args,
                       capture_output=True, text=True)
    text = open(out).read() if os.path.exists(out) else ''
    return r, text


def volts(text, node):
    """Spannungen der beiden Fragmente in Reihenfolge little, big."""
    import re
    frags = text.split('fragment@')
    return [re.findall(r'opp-microvolt = <(\d+)>;', f) for f in frags[1:]]


def main():
    with tempfile.TemporaryDirectory() as d:
        # Ohne Argumente: Werksspannung little 761, big 731.
        o1 = os.path.join(d, 'stock.dts')
        r, t = gen([], o1)
        check(r.returncode == 0, 'Standardlauf ok')
        check('= 28 OPPs' in r.stdout, '28 OPPs gemeldet')
        v = volts(t, None)
        check(len(v) == 2, 'zwei Fragmente')
        check(set(v[0]) == {'761000'}, 'little-Werksspannung 761 mV')
        check(set(v[1]) == {'731000'}, 'big-Werksspannung 731 mV')

        # Rueckwaerts-Kompatibilitaet: Standardlauf == die committete Datei.
        committed = open(os.path.join(HERE, 'lowfreq.dts')).read()
        check(open(o1).read() == committed, 'Standardlauf identisch zur Datei')

        # Explizite Werte je Cluster.
        o2 = os.path.join(d, 'uv.dts')
        r, t = gen(['--big-mv', '690', '--little-mv', '700'], o2)
        check(r.returncode == 0, 'Undervolt-Lauf ok')
        v = volts(t, None)
        check(set(v[0]) == {'700000'}, 'little explizit 700 mV')
        check(set(v[1]) == {'690000'}, 'big explizit 690 mV')
        check('big 690 mV' in r.stdout and 'little 700 mV' in r.stdout,
              'Spannungen in der Ausgabe')

        # Nur ein Cluster explizit: der andere bleibt auf Werksspannung.
        o3 = os.path.join(d, 'half.dts')
        r, t = gen(['--big-mv', '700'], o3)
        v = volts(t, None)
        check(set(v[0]) == {'761000'}, 'little bleibt Werksspannung')
        check(set(v[1]) == {'700000'}, 'big explizit')

        # Unter dem Regler-Boden: muss abgelehnt werden, keine Datei.
        o4 = os.path.join(d, 'bad.dts')
        r, t = gen(['--big-mv', '670'], o4)
        check(r.returncode != 0, '670 mV abgelehnt')
        check('Regler-Grenze' in (r.stdout + r.stderr), 'Grund genannt')
        check(not os.path.exists(o4), 'keine Datei bei Fehler')

        # Genau der Boden ist erlaubt.
        o5 = os.path.join(d, 'floor.dts')
        r, t = gen(['--big-mv', '680', '--little-mv', '680'], o5)
        check(r.returncode == 0, '680 mV (Boden) erlaubt')

        # Jede Rate des Overlays traegt die Zielspannung.
        v = volts(t, None)
        check(len(v[0]) == 14 and len(v[1]) == 14, '14 Raten je Cluster')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
