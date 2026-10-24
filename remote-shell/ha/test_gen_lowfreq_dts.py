#!/usr/bin/env python3
"""Host-Tests fuer gen_lowfreq_dts.py (reine Erzeugung, kein Board, kein dtc).

Der Generator ist die einzige Stelle, an der eine Spannung dauerhaft in die
OPP-Tabelle geschrieben wird. Ein zu niedriger Wert hier kann das Board in
einen Boot-Loop schicken, deshalb wird die Spannungslogik geprueft: Werkswert
ohne Argument, expliziter Wert je Cluster, die Spannungs-KURVE je Takt
(inline und JSON), Ablehnung unter dem Regler-Boden, die Reserve-Warnung unter
695 mV, Ablehnung unbekannter Raten / zu vieler Raten und die
Rueckwaerts-Kompatibilitaet des Standardausgabe.
"""

import json
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


def rate_volts(text, frag):
    """{rate_hz: uv} fuer ein Fragment (0 = little, 1 = big)."""
    import re
    body = text.split('fragment@')[frag + 1]
    return {int(h): int(u) for h, u in
            re.findall(r'opp-hz = /bits/ 64 <(\d+)>;\s*\n\s*'
                       r'opp-microvolt = <(\d+)>;', body)}


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
        # Auf dem Board liegt lowfreq.dts nicht neben dem Skript; dann entfaellt
        # nur dieser eine Vergleich, die Spannungspruefungen laufen weiter.
        dts = os.path.join(HERE, 'lowfreq.dts')
        if os.path.exists(dts):
            check(open(o1).read() == open(dts).read(),
                  'Standardlauf identisch zur Datei')
        else:
            print('  Hinweis: lowfreq.dts nicht neben dem Skript - '
                  'Dateivergleich uebersprungen')

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

        # --- Spannungs-KURVE je Takt -------------------------------------
        # Inline: eine eigene Spannung pro Rate, nicht genannte Raten bleiben
        # auf Werksspannung. little bekommt 761, big eine echte Kurve.
        o6 = os.path.join(d, 'curve.dts')
        r, t = gen(['--big-table', '24000=700,50000=690,666666=730'], o6)
        check(r.returncode == 0, 'Inline-Kurve ok')
        b = rate_volts(t, 1)
        l = rate_volts(t, 0)
        check(b[24000000] == 700000, 'Kurve 24 MHz -> 700 mV')
        check(b[50000000] == 690000, 'Kurve 50 MHz -> 690 mV')
        check(b[666666666] == 730000, 'Kurve 666 MHz -> 730 mV')
        check(b[100000000] == 731000, 'nicht genannte Rate bleibt Werksspannung')
        check(set(l.values()) == {761000}, 'little unberuehrt von --big-table')
        check('Kurve' in r.stdout, 'Kurve in der Ausgabe')

        # JSON-Datei, flach.
        flat = os.path.join(d, 'flat.json')
        with open(flat, 'w') as f:
            json.dump({'24000': 705, '50000': 695}, f)
        o7 = os.path.join(d, 'flat.dts')
        r, t = gen(['--big-table', '@' + flat], o7)
        check(r.returncode == 0, 'JSON-Kurve (flach) ok')
        b = rate_volts(t, 1)
        check(b[24000000] == 705000 and b[50000000] == 695000,
              'JSON-Werte greifen')

        # JSON-Datei, nach Cluster verschachtelt: jede Tabelle zieht nur ihren
        # Cluster.
        nest = os.path.join(d, 'nest.json')
        with open(nest, 'w') as f:
            json.dump({'big': {'24000': 710}, 'little': {'24000': 720}}, f)
        o8 = os.path.join(d, 'nest.dts')
        r, t = gen(['--big-table', '@' + nest, '--little-table', '@' + nest], o8)
        check(r.returncode == 0, 'JSON-Kurve (verschachtelt) ok')
        check(rate_volts(t, 1)[24000000] == 710000, 'big aus verschachteltem JSON')
        check(rate_volts(t, 0)[24000000] == 720000, 'little aus verschachteltem JSON')

        # Kurve unter dem Boden: ablehnen, keine Datei.
        o9 = os.path.join(d, 'curvebad.dts')
        r, t = gen(['--big-table', '24000=670'], o9)
        check(r.returncode != 0, 'Kurve unter Boden abgelehnt')
        check('Regler-Grenze' in (r.stdout + r.stderr), 'Grund genannt')
        check(not os.path.exists(o9), 'keine Datei bei Kurven-Fehler')

        # Genau der Boden in der Kurve ist erlaubt.
        o10 = os.path.join(d, 'curvefloor.dts')
        r, t = gen(['--big-table', '24000=680'], o10)
        check(r.returncode == 0, 'Kurve genau am Boden erlaubt')

        # Reserve-Warnung: < 695 mV erlaubt, aber Warnung auf stderr.
        o11 = os.path.join(d, 'warn.dts')
        r, t = gen(['--big-table', '24000=690'], o11)
        check(r.returncode == 0, '690 mV in Kurve erlaubt')
        check('WARNUNG' in r.stderr and 'Reserve' in r.stderr,
              'Reserve-Warnung bei <695 mV')
        # 695 mV ist die Grenze - keine Warnung mehr.
        o12 = os.path.join(d, 'nowarn.dts')
        r, t = gen(['--big-table', '24000=695'], o12)
        check(r.returncode == 0, '695 mV erlaubt')
        check('WARNUNG' not in r.stderr, 'keine Warnung bei 695 mV')
        # Die alte --mv-Variante warnt genauso.
        o13 = os.path.join(d, 'mvwarn.dts')
        r, t = gen(['--big-mv', '690'], o13)
        check('WARNUNG' in r.stderr, 'Warnung auch bei --big-mv')

        # Unbekannte Rate: ablehnen.
        o14 = os.path.join(d, 'unknown.dts')
        r, t = gen(['--big-table', '12345=700'], o14)
        check(r.returncode != 0, 'unbekannte Rate abgelehnt')
        check('unbekannte Rate' in (r.stdout + r.stderr), 'Grund genannt')
        check(not os.path.exists(o14), 'keine Datei bei unbekannter Rate')

        # Mehr als 14 Raten: ablehnen.
        many = ','.join('%d=700' % (i * 1000) for i in range(1, 16))
        o15 = os.path.join(d, 'many.dts')
        r, t = gen(['--big-table', many], o15)
        check(r.returncode != 0, '15 Raten abgelehnt')
        check('hoechstens 14' in (r.stdout + r.stderr), 'Grund genannt')

        # --mv und --table zusammen: ablehnen.
        o16 = os.path.join(d, 'both.dts')
        r, t = gen(['--big-mv', '700', '--big-table', '24000=700'], o16)
        check(r.returncode != 0, '--mv und --table zusammen abgelehnt')

        # Regression: gen_oc_dts.py ruft lf.render() auf. Die Kurven-Erweiterung
        # darf die Signatur nicht entfernen, sonst bricht der OC-Pfad
        # (gen_oc_dts.py --with-lowfreq) beim Import/Aufruf ab.
        oc = os.path.join(d, 'oc.dts')
        r = subprocess.run([sys.executable, os.path.join(HERE, 'gen_oc_dts.py'),
                            '--rate-mhz', '2472', '--with-lowfreq',
                            '--big-mv', '700', '--out', oc],
                           capture_output=True, text=True)
        check(r.returncode == 0, 'gen_oc_dts --with-lowfreq laeuft (render() bleibt)')
        ocv = volts(open(oc).read(), None) if os.path.exists(oc) else []
        check(len(ocv) == 2 and '2472000000' in open(oc).read(),
              'OC-Overlay: Tief-Takte + 2472-MHz-Punkt')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
