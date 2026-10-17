#!/usr/bin/env python3
"""Kalibrierung: nominale DT-Spannung <-> REALE Rail-Spannung (big-Cluster).

Messung (ADC ch4, mean-Kanal, Referenz kalibriert: gnd=0, 0.5vdd=900.0 mV):

    nominal (mV)   duty   real (mV)
        680          0       725.1
        700         56       740.5
        731        142       766.8
        760        222       791.0
        800        333       824.0
        900        610       905.3
       1040        999      1022.2

Der cpufreq-Treiber rechnet mit dem DT-Modell
    duty = (nominal - 680000) * period / 360000
und erwartet real = nominal. Gemessen gilt aber eine AFFINE Beziehung mit
anderem Offset UND anderer Steigung:

    real_mV = 724.6 + 0.8255 * (nominal_mV - 680)

Steigung 0.8255 (nicht 1.0) und Offset +44.6 mV: der PWM-Regler bildet das
Tastverhaeltnis also nicht 1:1 ab. Folge:

  * Der DT-Boden 680 mV wird NIE erreicht - real sind es ~724.6 mV.
  * Bei duty 0 ist Schluss: es gibt kein duty < 0. Die reale Untergrenze ist
    damit ~724.6 mV, unabhaengig davon, welches nominal man deklariert.
  * Um eine REALE Zielspannung zu treffen, muss man das nominale
    opp-microvolt vorverzerren: nominal = 680 + (real - 724.6) / 0.8255.
"""
import argparse

REAL_AT_FLOOR = 724.6      # real mV bei duty 0
GAIN = 0.8255              # d(real)/d(nominal)
DT_MIN = 680.0
DT_MAX = 1040.0
PERIOD_BIG = 999

MEASURED = [  # (nominal, duty, real)
    (680, 0, 725.1), (700, 56, 740.5), (731, 142, 766.8), (760, 222, 791.0),
    (800, 333, 824.0), (900, 610, 905.3), (1040, 999, 1022.2),
]


def nominal_to_real(nom_mv):
    return REAL_AT_FLOOR + GAIN * (nom_mv - DT_MIN)


def real_to_nominal(real_mv):
    return DT_MIN + (real_mv - REAL_AT_FLOOR) / GAIN


def real_floor():
    """Reale Spannung, die der DT-Boden (duty 0) physisch erzeugt."""
    return nominal_to_real(DT_MIN)


def self_test():
    worst = 0.0
    for nom, duty, real in MEASURED:
        pred = nominal_to_real(nom)
        worst = max(worst, abs(pred - real))
    # Round-trip
    for nom in (680, 731, 900, 1040):
        rt = real_to_nominal(nominal_to_real(nom))
        worst = max(worst, abs(rt - nom))
    return worst


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--real', type=float, action='append', default=[],
                    help='reale Zielspannung in mV; gibt das noetige nominal aus')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()

    if args.selftest or not args.real:
        print('max. Modellabweichung: %.2f mV' % self_test())
        print('realer Boden (duty 0):  %.1f mV' % real_floor())
        print('Werks-Tief-Takt nominal 731 -> real %.1f mV' % nominal_to_real(731))
        print('reale Ersparnis Boden:  %.1f mV' % (nominal_to_real(731) - real_floor()))
        print()
        print(' nominal -> real          real -> nominal')
        for nom in (680, 700, 731, 760, 800, 900, 1040):
            print('  %5d -> %7.1f mV' % (nom, nominal_to_real(nom)), end='')
            print('     %7.1f -> %6.1f mV' % (nominal_to_real(nom),
                                              real_to_nominal(nominal_to_real(nom))))
    for r in args.real:
        nom = real_to_nominal(r)
        d = round((nom - DT_MIN) * PERIOD_BIG / (DT_MAX - DT_MIN))
        ok = 'OK' if nom >= DT_MIN - 1e-6 else 'UNTER BODEN (unmoeglich)'
        print('reale %.1f mV  ->  nominal opp-microvolt = %d uV  (duty %d)  [%s]'
              % (r, round(nom * 1000), d, ok))


if __name__ == '__main__':
    main()
