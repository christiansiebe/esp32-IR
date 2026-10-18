#!/usr/bin/env python3
"""Host-Tests fuer gen_oc_dts.py (reine Erzeugung, kein Board, kein dtc).

Geprueft werden die drei Zusagen des OC-Generators:
  * der gemessene stabile Punkt 2496 MHz (N=208) wird erzeugt,
  * Werte unter dem Regler-Boden (680 mV) werden abgelehnt,
  * Raten, die der big-PLL nicht erzeugen kann, werden abgelehnt,
  * der Standardlauf von gen_lowfreq_dts.py bleibt byte-identisch zu lowfreq.dts.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, 'gen_oc_dts.py')
LF_GEN = os.path.join(HERE, 'gen_lowfreq_dts.py')

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


def opp_list(text):
    """Liste (rate_hz, microvolt) aller OPP-Punkte in Dateireihenfolge."""
    return [(int(r), int(v)) for r, v in
            re.findall(r'opp-hz = /bits/ 64 <(\d+)>;\s*\n\s*opp-microvolt = <(\d+)>;',
                       text)]


def opps(text):
    """{rate_hz: microvolt}; bei gleicher Rate je Cluster kollabiert der Wert."""
    return dict(opp_list(text))


def main():
    with tempfile.TemporaryDirectory() as d:
        # 1. Standardlauf: genau der gemessene Punkt, 2496 MHz @ 1040 mV.
        o1 = os.path.join(d, 'oc.dts')
        r, t = gen([], o1)
        check(r.returncode == 0, 'Standardlauf ok')
        o = opps(t)
        check(o == {2496000000: 1040000}, 'genau ein Punkt: 2496 MHz @ 1040 mV')
        check('opp-table-1' in t, 'Ziel ist der big-Cluster (opp-table-1)')
        check('N=208' in t, 'N=208 dokumentiert')
        check('2496 MHz (N=208) @ 1040 mV' in r.stdout, 'Ausgabe nennt Punkt')

        # 2. Regler-Boden: 670 mV muss abgelehnt werden, ohne Datei.
        o2 = os.path.join(d, 'bad.dts')
        r, t = gen(['--mv', '670'], o2)
        check(r.returncode != 0, '670 mV abgelehnt')
        check('Regler-Grenze' in (r.stdout + r.stderr), 'Grund genannt')
        check(not os.path.exists(o2), 'keine Datei bei Fehler')

        # 3. Genau der Boden (680 mV) ist erlaubt.
        o3 = os.path.join(d, 'floor.dts')
        r, t = gen(['--mv', '680'], o3)
        check(r.returncode == 0, '680 mV (Boden) erlaubt')
        check(opps(t) == {2496000000: 680000}, '680 mV uebernommen')

        # 4. Nicht darstellbare Rate (2450 MHz, N=204.17) wird abgelehnt.
        o4 = os.path.join(d, 'frac.dts')
        r, t = gen(['--rate-mhz', '2450'], o4)
        check(r.returncode != 0, '2450 MHz (nicht durch 12 teilbar) abgelehnt')
        check('nicht darstellbar' in (r.stdout + r.stderr), 'Grund genannt')
        check(not os.path.exists(o4), 'keine Datei bei nicht darstellbarer Rate')

        # 5. Andere gueltige Punkte werden korrekt umgesetzt.
        o5 = os.path.join(d, 'oc2520.dts')
        r, t = gen(['--rate-mhz', '2520', '--mv', '1040'], o5)
        check(r.returncode == 0, '2520 MHz erzeugt')
        check(opps(t) == {2520000000: 1040000}, '2520 MHz = N=210 @ 1040 mV')
        check('ACHTUNG' in r.stdout, 'Warnung oberhalb des Messpunkts')

        # 6. --with-lowfreq: OC-Punkt UND alle Tief-Takte.
        o6 = os.path.join(d, 'full.dts')
        r, t = gen(['--with-lowfreq', '--big-mv', '700', '--little-mv', '700'], o6)
        check(r.returncode == 0, '--with-lowfreq ok')
        o = opps(t)
        check(2496000000 in o, 'OC-Punkt enthalten')
        check(15625000 in o, 'tiefste Rate enthalten')
        check(len(opp_list(t)) == 29, '29 OPP-Eintraege (28 Tief-Takte + 1 OC)')
        check(o[2496000000] == 1040000 and o[15625000] == 700000,
              'Spannungen je Punkt korrekt')

        # 7. Standardlauf von gen_lowfreq_dts.py bleibt unveraendert.
        o7 = os.path.join(d, 'lf.dts')
        r = subprocess.run([sys.executable, LF_GEN, '--out', o7],
                           capture_output=True, text=True)
        check(r.returncode == 0, 'lowfreq-Standardlauf ok')
        dts = os.path.join(HERE, 'lowfreq.dts')
        if os.path.exists(dts):
            check(open(o7).read() == open(dts).read(),
                  'lowfreq-Standardlauf byte-identisch zu lowfreq.dts')
        else:
            print('  Hinweis: lowfreq.dts nicht neben dem Skript - '
                  'Dateivergleich uebersprungen')
        # Der OC-Generator darf lowfreq.dts nie anfassen: Standardziel ist oc.dts.
        check(not os.path.exists(os.path.join(HERE, 'oc.dts')) or
              open(os.path.join(HERE, 'lowfreq.dts')).read().count('2496000000') == 0,
              'lowfreq.dts enthaelt keinen OC-Punkt')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
