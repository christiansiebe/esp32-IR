#!/usr/bin/env python3
"""Erzeugt oc.dts: ein big-Cluster-OPP oberhalb der Werksgrenze (2472 MHz).

Getrennt von gen_lowfreq_dts.py, damit der Standardlauf die committete
`lowfreq.dts` NICHT anfasst. Dieses Skript schreibt nach `oc.dts` und nimmt
dort - optional zusaetzlich zu den Tief-Takten - den gemessenen Overclock-Punkt
auf:

    big-PLL: HHI 0xff63c000 + 0x2f4, Bits [8:0] = N, sys_pll = N * 12 MHz.
    Stock big max = 2400 MHz (N=200). Gemessen stabil: N=206 = 2472 MHz
    (30 min Soak, 22828 Laeufe, 0 Fehler). 2508/2520 grenzwertig, 2532 haengt.

Risiko: die Spannung liegt am Regler-Anschlag (100 % duty = 1040 mV). Oberhalb
von 2400 MHz laeuft alles UNTERSPANNT - es gibt keinen Spielraum, den Takt
ueber eine hoehere Spannung abzusichern. Das ist ein Experiment, kein
Dauerbetrieb. Siehe OC-README.md.

Der Punkt ist bei den Vorgaben bewusst NICHT das Governor-Maximum: die
mitgelieferte `oc-freq-guard.service` deckelt `scaling_max_freq` beim Boot auf
den Werkswert 2400 MHz, sodass der OC-Punkt nur durch ein ausdrueckliches
Anheben von `scaling_max_freq` erreichbar ist (Muster wie bei den Tief-Takten).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_lowfreq_dts as lf  # noqa: E402

# Gemessener stabiler Punkt (siehe PLL-OC-README.md / OC-README.md).
OC_RATE = 2472000000
OC_N = 206                      # OC_RATE / 12 MHz
OC_UV_DEFAULT = 1040000         # 100 % PWM-Duty = Regler-Anschlag
STOCK_MAX = 2400000000          # Werks-maximum des big-Clusters

REG_FLOOR_UV = lf.REG_FLOOR_UV  # 680000 - unter dem PWM-Regler geht es nicht


def pll_n(rate_hz):
    """Gibt N zurueck (rate_hz = N * 12 MHz) oder None, wenn nicht darstellbar.

    Der big-PLL kennt nur Vielfache von 12 MHz; Bits [8:0] begrenzen N auf 511.
    """
    if rate_hz % 12000000:
        return None
    n = rate_hz // 12000000
    return n if 0 < n <= 511 else None


HEAD = """\
/dts-v1/;
/plugin/;

/*
 * Adds an overclock OPP entry (big cluster, Cortex-A73 / opp-table-1) to the
 * ODROID-N2(+) device tree.
 *
 * The big CPU PLL (HHI 0xff63c000 + 0x2f4, bits [8:0] = N) runs at
 * sys_pll = N * 12 MHz. Stock big max is 2400 MHz (N=200). The point below is
 * the measured-stable overclock point:
 *
 *   %d MHz = N=%d (measured: 30 min soak, 22828 runs, 0 errors)
 *
 * The voltage is the regulator ceiling: at stock 2400 MHz the PWM regulator
 * already sits at 100 %% duty (1040 mV). Every point above 2400 MHz therefore
 * runs UNDERVOLTED relative to the OPP curve - there is no headroom to make a
 * higher clock safer with more voltage. This is an experiment, not a
 * permanent operating point.
 *
 * The point is deliberately not selected automatically: oc-freq-guard.service
 * caps scaling_max_freq at the stock 2400 MHz right after boot, so it is only
 * reachable by an explicit runtime raise of scaling_max_freq.
 */
"""


def render_oc_only(rate_hz, uv):
    """Nur der OC-Punkt, gegen /opp-table-1 (big)."""
    return (HEAD % (rate_hz // 1000000, rate_hz // 12000000) +
            '\n/ {\n\tfragment@0 {\n\t\ttarget-path = "/opp-table-1";\n'
            '\t\t__overlay__ {\n'
            '\t\t\topp-%d {\n'
            '\t\t\t\topp-hz = /bits/ 64 <%d>;\n'
            '\t\t\t\topp-microvolt = <%d>;\n'
            '\t\t\t\tclock-latency-ns = <50000>;\n'
            '\t\t\t};\n'
            '\t\t};\n\t};\n};\n' % (rate_hz, rate_hz, uv))


def render_full(rate_hz, uv, big_mv=None, little_mv=None):
    """Tief-Takte (aus gen_lowfreq_dts) + OC-Punkt in einem Overlay."""
    note = ('\n/*\n * Plus the overclock point %d MHz (N=%d) on the big cluster.\n'
            ' * See the top of this file and OC-README.md for the risk.\n */\n'
            % (rate_hz // 1000000, rate_hz // 12000000))
    return lf.render(big_mv, little_mv,
                     extra_big=[(rate_hz, uv, 'OC')], header_note=note)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mv', type=int, default=OC_UV_DEFAULT // 1000,
                    help='opp-microvolt des OC-Punkts in mV (Standard %d = '
                         'Regler-Anschlag)' % (OC_UV_DEFAULT // 1000))
    ap.add_argument('--rate-mhz', type=int, default=OC_RATE // 1000000,
                    help='OC-Frequenz in MHz (Standard %d, gemessen stabil)'
                         % (OC_RATE // 1000000))
    ap.add_argument('--with-lowfreq', action='store_true',
                    help='zusaetzlich die Tief-Takte unter 1 GHz aufnehmen '
                         '(vollstaendiges Overlay)')
    ap.add_argument('--big-mv', type=int, default=None,
                    help='Spannung der big-Tief-Takte (nur mit --with-lowfreq)')
    ap.add_argument('--little-mv', type=int, default=None,
                    help='Spannung der little-Tief-Takte (nur mit --with-lowfreq)')
    ap.add_argument('--out', default=os.path.join(HERE, 'oc.dts'))
    args = ap.parse_args()

    uv = args.mv * 1000
    rate = args.rate_mhz * 1000000
    n = pll_n(rate)
    if n is None:
        raise SystemExit('%d MHz ist nicht darstellbar: der big-PLL erzeugt nur '
                         'Vielfache von 12 MHz (N=1..511).' % args.rate_mhz)
    if uv < REG_FLOOR_UV:
        raise SystemExit('big: %d mV liegt unter der Regler-Grenze %d mV'
                         % (args.mv, REG_FLOOR_UV // 1000))

    if args.with_lowfreq:
        text = render_full(rate, uv, args.big_mv, args.little_mv)
        extra = ' + %d Tief-Takte' % (len(lf.RATES) * len(lf.CLUSTERS))
    else:
        text = render_oc_only(rate, uv)
        extra = ''
    with open(args.out, 'w') as f:
        f.write(text)
    warn = ''
    if rate > OC_RATE:
        warn = '  ACHTUNG: oberhalb von %d MHz grenzwertig/instabil!' % (OC_RATE // 1000000)
    print('%s geschrieben: big %d MHz (N=%d) @ %d mV%s%s'
          % (args.out, args.rate_mhz, n, args.mv, extra, warn))


if __name__ == '__main__':
    main()
