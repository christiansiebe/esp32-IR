#!/usr/bin/env python3
"""Erzeugt lowfreq.dts: OPP-Eintraege unter 1 GHz fuer beide Cluster.

Optional kann fuer die neuen Tief-Takte eine niedrigere Spannung eingetragen
werden (`--big-mv` / `--little-mv`). Das ist der EINZIGE Weg zu einem
dauerhaften Undervolt: die OPP-Tabelle ist zur Laufzeit read-only, der
PWM-Regler laesst sich nur ueber /dev/mem erreichen (nicht reboot-fest), und
`cpufreq-dt` setzt bei jedem Frequenzwechsel die Spannung des OPP-Punkts neu.

Achtung Boot-Loop-Risiko: seit die Untergrenze dauerhaft auf 15.625 MHz steht,
erreicht `schedutil` die Tief-Takte schon kurz nach dem Boot. Ist die Spannung
zu niedrig, haengt das Board, der Watchdog rebootet - und laeuft in dieselbe
Lage. Deshalb Vorsicht mit dem Wert und immer Reserve lassen.
"""
import argparse
import os

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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--big-mv', type=int, default=None,
                    help='Spannung der big-Tief-Takte in mV (Werkswert 731)')
    ap.add_argument('--little-mv', type=int, default=None,
                    help='Spannung der little-Tief-Takte in mV (Werkswert 761)')
    ap.add_argument('--out', default=os.path.join(HERE, 'lowfreq.dts'))
    args = ap.parse_args()

    want = {'big': args.big_mv, 'little': args.little_mv}
    tbl = '\n'.join(' *   %-11d = %s' % (r, c) for r, c in RATES)
    out = [HEAD % tbl]
    for i, (node, label, stock_uv, key) in enumerate(CLUSTERS):
        uv = want[key] * 1000 if want[key] else stock_uv
        if uv < REG_FLOOR_UV:
            raise SystemExit('%s: %d mV liegt unter der Regler-Grenze %d mV'
                             % (key, uv // 1000, REG_FLOOR_UV // 1000))
        out.append('\n/ {\n\tfragment@%d {\n\t\ttarget-path = "/%s";\n\t\t__overlay__ {\n'
                   % (i, node))
        for r, c in RATES:
            out.append('\t\t\topp-%d {\n'
                       '\t\t\t\topp-hz = /bits/ 64 <%d>;\n'
                       '\t\t\t\topp-microvolt = <%d>;\n'
                       '\t\t\t\tclock-latency-ns = <50000>;\n'
                       '\t\t\t};\n' % (r, r, uv))
        out.append('\t\t};\n\t};\n};\n')
    with open(args.out, 'w') as f:
        f.write(''.join(out))
    vols = ', '.join('%s %d mV' % (k, (want[k] * 1000 if want[k] else
                                       [c[2] for c in CLUSTERS if c[3] == k][0]) // 1000)
                     for k in ('big', 'little'))
    print('%s geschrieben: %d Raten x %d Cluster = %d OPPs (%s)'
          % (args.out, len(RATES), len(CLUSTERS), len(RATES) * len(CLUSTERS), vols))


if __name__ == '__main__':
    main()
