#!/usr/bin/env python3
"""Erzeugt lowfreq.dts: OPP-Eintraege unter 1 GHz fuer beide Cluster.

Optional kann fuer die neuen Tief-Takte eine niedrigere Spannung eingetragen
werden. Das ist der EINZIGE Weg zu einem dauerhaften Undervolt: die
OPP-Tabelle ist zur Laufzeit read-only, der PWM-Regler laesst sich nur ueber
/dev/mem erreichen (nicht reboot-fest), und `cpufreq-dt` setzt bei jedem
Frequenzwechsel die Spannung des OPP-Punkts neu.

Es gibt zwei Wege:

* `--big-mv` / `--little-mv` setzen EINEN Wert fuer alle Tief-Takte des
  Clusters (der historische, einfache Fall).
* `--big-table` / `--little-table` setzen eine SPANNUNGS-KURVE, also eine
  eigene Spannung pro Takt. Der Wert ist entweder eine Inline-Liste
  `RATE=VALUE,...` (RATE in kHz, VALUE in mV, z.B. `24000=700,50000=690`)
  oder der Pfad einer JSON-Datei (mit `@` davor erzwungen, z.B.
  `@curve.json`). Die JSON-Datei ist entweder ein flaches Objekt
  `{"24000": 700, "50000": 690}` oder nach Cluster verschachtelt
  `{"big": {...}, "little": {...}}`. Nicht genannte Raten behalten die
  Werksspannung des 1.0-GHz-Punkts.

Fuer beide Wege gilt dieselben Schutzgrenzen: Werte unter der Regler-Untergrenze
680 mV werden abgelehnt; Werte unter 695 mV sind erlaubt, werden aber als
fehlende Reserve deutlich gemeldet. Unbekannte Raten (nicht im Overlay) und
mehr als 14 Raten werden abgelehnt.

Achtung Boot-Loop-Risiko: seit die Untergrenze dauerhaft auf 15.625 MHz steht,
erreicht `schedutil` die Tief-Takte schon kurz nach dem Boot. Ist die Spannung
zu niedrig, haengt das Board, der Watchdog rebootet - und laeuft in dieselbe
Lage. Deshalb Vorsicht mit dem Wert und immer Reserve lassen.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# (rate_hz, kommentar) - alle Werte sind mux-parent/teiler aus dem meson-Taktbaum:
#   xtal = 24 MHz, fclk_div2 = 1.0 GHz, fclk_div3 = 666.67 MHz.
#
# Gemessen (cpu_clk_dyn, der tatsaechliche CPU-Takt): der Teiler kennt nur Werte
# der Form 2^a * 5^b. Eine Rate mit Faktor 3 im Teiler wird still auf den
# naechsten erreichbaren Wert ABgerundet - 333333 kHz lief real mit 250000 kHz,
# 33333 kHz real mit 25000 kHz, obwohl scaling_cur_freq die Wunschrate meldete.
# Solche Punkte sind deshalb nicht in der Tabelle: cpufreq wuerde eine falsche
# Frequenz melden und die Spannung zum falschen Punkt waehlen.
RATES = [
    (666666666, 'fclk_div3 / 1'),
    (500000000, 'fclk_div2 / 2'),
    (250000000, 'fclk_div2 / 4'),
    (200000000, 'fclk_div2 / 5'),
    (125000000, 'fclk_div2 / 8'),
    (100000000, 'fclk_div2 / 10'),
    (62500000, 'fclk_div2 / 16'),
    (50000000, 'fclk_div2 / 20'),
    (40000000, 'fclk_div2 / 25'),
    (31250000, 'fclk_div2 / 32'),
    (25000000, 'fclk_div2 / 40'),
    (20000000, 'fclk_div2 / 50'),
    (15625000, 'fclk_div2 / 64'),
    (24000000, 'xtal / 1'),
]

# Werksspannung bei 1.0 GHz je Cluster (aus dem PWM-Regler gelesen).
CLUSTERS = [
    ('opp-table-0', 'little (Cortex-A53, policy0)', 761000, 'little'),
    ('opp-table-1', 'big (Cortex-A73, policy2)', 731000, 'big'),
]

# Regler-Untergrenze (regulator.{2,8}/min_microvolts) - tiefer geht das PWM nicht.
REG_FLOOR_UV = 680000

# Unter dieser Spannung ist der Abstand zum Regler-Boden so klein, dass keine
# Reserve fuer Fertigungsstreuung/Temperatur bleibt -> erlaubt, aber warnen.
RESERVE_WARN_UV = 695000

# kHz-Rate -> Hz, fuer die Schluessel der Spannungskurve.
RATE_KHZ = {r // 1000: r for r, _ in RATES}

HEAD = """\
/dts-v1/;
/plugin/;

/*
 * Adds CPU OPP entries below 1 GHz to the ODROID-N2(+) device tree.
 *
 * The stock opp-table-0 (Cortex-A53 "little", policy0) and opp-table-1
 * (Cortex-A73 "big", policy2) start at 1.0 GHz, so cpufreq cannot select
 * anything slower even though the meson clock tree can. cpu_clk_dyn0_sel muxes
 * xtal, fclk_div2 and fclk_div3 through a 6-bit divider, so every rate below is
 * parent/div exactly:
 *
%s
 *
 * 24 MHz is the hard floor: xtal is the lowest mux parent, and it is also the
 * driver's own parking clock, so the core demonstrably runs there. Rates below
 * it are not reachable through this mux.
 *
 * The voltages are the stock 1.0 GHz values: this overlay only unlocks the
 * frequency, undervolting is a separate experiment. They also sit well above
 * the regulator floor (680 mV).
 *
 * These points are NOT reachable at boot: cpufreq.default_governor=performance
 * keeps early boot at maximum frequency and cpu-freq-guard.sh pins
 * scaling_min_freq to 1.0 GHz, so a bad low point can only be hit by an
 * explicit runtime probe - a reset then restores the floor instead of looping.
 */
"""


def _parse_pairs(spec):
    """Inline-Spezifikation 'RATE=VALUE,...' -> {rate_khz: mv}."""
    out = {}
    for item in spec.split(','):
        item = item.strip()
        if not item:
            continue
        if '=' not in item:
            raise ValueError('Eintrag ohne = : %r' % item)
        rk, mv = item.split('=', 1)
        try:
            out[int(rk.strip())] = int(mv.strip())
        except ValueError:
            raise ValueError('RATE=VALUE erwartet (kHz=mV), bekam %r' % item)
    return out


def _load_json(path):
    try:
        with open(path) as f:
            data = json.load(f)
    except OSError as e:
        raise ValueError('Datei nicht lesbar: %s' % e)
    except json.JSONDecodeError as e:
        raise ValueError('kein gueltiges JSON: %s' % e)
    if not isinstance(data, dict):
        raise ValueError('JSON-Wurzel muss ein Objekt sein')
    return data


def parse_curve(spec, key):
    """Spec -> {rate_khz: mv}. `key` waehlt bei verschachteltem JSON den Cluster.

    Inline: 'RATE=VALUE,...' (kHz=mV). Mit '@' davor: JSON-Datei, entweder
    flach {RATE: mV} oder nach Cluster verschachtelt {'big': {...}, ...}.
    """
    if not spec.startswith('@'):
        return _parse_pairs(spec)
    data = _load_json(spec[1:])
    if {'big', 'little'} & set(data):
        if key not in data or not isinstance(data[key], dict):
            raise ValueError('JSON hat keine Tabelle fuer %s' % key)
        data = data[key]
    pairs = {}
    for k, v in data.items():
        try:
            pairs[int(k)] = int(v)
        except (TypeError, ValueError):
            raise ValueError('JSON-Schluessel/Wert nicht numerisch: %r=%r' % (k, v))
    return pairs


def build_curve(key, mv, table):
    """{rate_hz: uv} fuer einen Cluster. Ohne Argument die Werksspannung."""
    if mv is not None and table is not None:
        raise SystemExit('%s: --%s-mv und --%s-table nicht zusammen angeben'
                         % (key, key, key))
    stock_uv = next(c[2] for c in CLUSTERS if c[3] == key)
    if mv is not None:
        return {r: mv * 1000 for r, _ in RATES}
    if table is None:
        return {r: stock_uv for r, _ in RATES}
    try:
        pairs = parse_curve(table, key)
    except ValueError as e:
        raise SystemExit('%s: %s' % (key, e))
    if len(pairs) > len(RATES):
        raise SystemExit('%s: %d Raten in der Kurve, hoechstens %d erlaubt'
                         % (key, len(pairs), len(RATES)))
    curve = {r: stock_uv for r, _ in RATES}
    for rk, mvv in pairs.items():
        if rk not in RATE_KHZ:
            raise SystemExit('%s: unbekannte Rate %d kHz (erlaubt: %s)'
                             % (key, rk, ' '.join(str(x) for x in sorted(RATE_KHZ))))
        curve[RATE_KHZ[rk]] = mvv * 1000
    return curve


def check_curve(key, curve):
    """Wirft unter dem Regler-Boden, meldet fehlende Reserve als Warnungen."""
    warnings = []
    for r in sorted(curve):
        uv = curve[r]
        if uv < REG_FLOOR_UV:
            raise SystemExit('%s: %d kHz -> %d mV liegt unter der Regler-Grenze %d mV'
                             % (key, r // 1000, uv // 1000, REG_FLOOR_UV // 1000))
        if uv < RESERVE_WARN_UV:
            warnings.append('%s: %d kHz -> %d mV hat kaum Reserve zum '
                            'Regler-Boden %d mV' % (key, r // 1000, uv // 1000,
                                                    REG_FLOOR_UV // 1000))
    return warnings


def describe(key, curve, mv):
    if mv is not None:
        return '%s %d mV' % (key, mv)
    vals = sorted(set(curve.values()))
    if len(vals) == 1:
        return '%s %d mV' % (key, vals[0] // 1000)
    return '%s Kurve %d-%d mV (%d Punkte)' % (key, vals[0] // 1000,
                                              vals[-1] // 1000, len(curve))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--big-mv', type=int, default=None,
                    help='Spannung der big-Tief-Takte in mV (Werkswert 731)')
    ap.add_argument('--little-mv', type=int, default=None,
                    help='Spannung der little-Tief-Takte in mV (Werkswert 761)')
    ap.add_argument('--big-table', default=None,
                    help='Spannungs-Kurve big: "RATE=VALUE,..." (kHz=mV) '
                         'oder "@datei.json"')
    ap.add_argument('--little-table', default=None,
                    help='Spannungs-Kurve little: "RATE=VALUE,..." (kHz=mV) '
                         'oder "@datei.json"')
    ap.add_argument('--out', default=os.path.join(HERE, 'lowfreq.dts'))
    args = ap.parse_args()

    mv = {'big': args.big_mv, 'little': args.little_mv}
    table = {'big': args.big_table, 'little': args.little_table}
    curves = {}
    for _, _, _, key in CLUSTERS:
        curves[key] = build_curve(key, mv[key], table[key])

    warnings = []
    for _, _, _, key in CLUSTERS:
        warnings += check_curve(key, curves[key])
    for w in warnings:
        print('WARNUNG: %s' % w, file=sys.stderr)

    tbl = '\n'.join(' *   %-11d = %s' % (r, c) for r, c in RATES)
    out = [HEAD % tbl]
    for i, (node, label, stock_uv, key) in enumerate(CLUSTERS):
        out.append('\n/ {\n\tfragment@%d {\n\t\ttarget-path = "/%s";\n\t\t__overlay__ {\n'
                   % (i, node))
        for r, c in RATES:
            out.append('\t\t\topp-%d {\n'
                       '\t\t\t\topp-hz = /bits/ 64 <%d>;\n'
                       '\t\t\t\topp-microvolt = <%d>;\n'
                       '\t\t\t\tclock-latency-ns = <50000>;\n'
                       '\t\t\t};\n' % (r, r, curves[key][r]))
        out.append('\t\t};\n\t};\n};\n')
    with open(args.out, 'w') as f:
        f.write(''.join(out))
    vols = ', '.join(describe(k, curves[k], mv[k]) for k in ('big', 'little'))
    print('%s geschrieben: %d Raten x %d Cluster = %d OPPs (%s)'
          % (args.out, len(RATES), len(CLUSTERS), len(RATES) * len(CLUSTERS), vols))


if __name__ == '__main__':
    main()
