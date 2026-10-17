#!/usr/bin/env python3
"""Host-Tests fuer pwm_reg_model.py (reine Logik, kein Board, kein Netz).

Prueft die Nachbildung des Linux-`pwm-regulator` gegen die Formeln aus dem
Treiber-Quelltext und gegen die im Repo am Board gemessenen Eckwerte
(UV-README.md): bei 680 mV ist das Register-LOW-Feld 0, bei 1040 mV ist es die
Periodenlaenge, und die Rueckrechnung trifft den Sollwert.

Aufruf:  python3 test_pwm_reg_model.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pwm_reg_model as m  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('FAIL: %s' % msg)


def main():
    # --- Reglerknoten (extern, N2+) ---
    check(m.N2P['min_uv'] == 680000 and m.N2P['max_uv'] == 1040000,
          'N2+ min/max = 680/1040 mV')
    check(m.N2P['duty_min'] == 100 and m.N2P['duty_max'] == 0,
          'N2+ dutycycle-range = <100 0> (invertiert)')
    check(abs(m.step_mv(m.N2P) - 3.6) < 1e-9, 'N2+ Aufloesung = 3.6 mV/Schritt')
    check(abs(m.step_mv(m.N2) - 3.01) < 0.01, 'N2 Aufloesung = 3.01 mV/Schritt')

    # --- Continuous: Endpunkte muessen die invertierte Range treffen ---
    check(m.continuous_rel_duty(680000, **m.N2P) == 100,
          'min_uV 680 mV -> rel_duty 100')
    check(m.continuous_rel_duty(1040000, **m.N2P) == 0,
          'max_uV 1040 mV -> rel_duty 0')
    # Monoton fallend: mehr Spannung -> kleinerer relativer Tastgrad.
    prev = 101
    for uv in range(680000, 1040001, 10000):
        rel = m.continuous_rel_duty(uv, **m.N2P)
        check(rel <= prev, 'rel_duty monoton fallend @ %d' % uv)
        prev = rel
    check(prev == 0, 'rel_duty endet bei 0')

    # --- Registerabbildung (meson): LOW-Feld = (1-rel/unit)*period ---
    # Repo-Messung: 680 mV -> duty 0, 1040 mV -> duty == period (big P=999).
    for name, period in (('big', 999), ('little', 333)):
        check(m.rel_to_reg_duty(m.continuous_rel_duty(680000, **m.N2P), period) == 0,
              '%s: 680 mV -> reg_duty 0' % name)
        check(m.rel_to_reg_duty(m.continuous_rel_duty(1040000, **m.N2P), period)
              == period, '%s: 1040 mV -> reg_duty == period' % name)

    # Repo-Messung (UV-README): V = 680000 + duty*360000/period gilt nur mit
    # reg_duty. Ein mittlerer Sollwert muss ueber reg_duty zurueckpassen.
    for uv in range(700000, 1040000, 20000):
        rel = m.continuous_rel_duty(uv, **m.N2P)
        reg = m.rel_to_reg_duty(rel, 999)
        model = m.N2P['min_uv'] + reg * 360000 / 999
        check(abs(model - uv) <= m.step_mv(m.N2P) * 1000,
              'reg_duty-Modell trifft %d mV (Diff %d)' % (uv, int(model - uv)))

    # --- Round-Trip rel_duty <-> uV (Treiber get_voltage) ---
    for uv in range(680000, 1040001, 5000):
        rel = m.continuous_rel_duty(uv, **m.N2P)
        back = m.continuous_uv(rel, **m.N2P)
        check(abs(back - uv) <= m.step_mv(m.N2P) * 1000,
              'Round-Trip %d -> %d' % (uv, back))

    # --- Continuous: min-Verschiebung koppelt ALLE Sollwerte ---
    shifted = dict(m.N2P, min_uv=720000)
    deltas = []
    for uv in (700000, 800000, 900000, 1000000):
        a = m.continuous_rel_duty(uv, **m.N2P)
        b = m.continuous_rel_duty(uv, **shifted)
        deltas.append(b - a)
    check(all(d != 0 for d in deltas), 'min-Verschiebung bewegt jeden Sollwert')
    check(deltas == sorted(deltas, reverse=True),
          'Verschiebung nimmt mit der Spannung ab (Steigungs-Effekt)')

    # --- Voltage-Table: exakter Treffer, kein Treffer, kein Klemmen ---
    table = [(680000, 100), (800000, 67), (900000, 39),
             (1000000, 11), (1040000, 0)]
    check(m.table_select(900000, 900000, table) == (900000, 39),
          'Table: exakter Treffer 900 mV -> duty 39')
    check(m.table_select(850000, 850000, table) is None,
          'Table: 850 mV ohne Eintrag -> None (-EINVAL)')
    # Kernregel: kleinster Wert INNERHALB [min,max]; mit Band min<max.
    check(m.table_select(800000, 1000000, table) == (800000, 67),
          'Table: Band waehlt kleinsten Wert >= min_uV')
    # Reihenfolge der Tabelle darf das Ergebnis nicht aendern.
    check(m.table_select(900000, 900000, list(reversed(table)))
          == (900000, 39), 'Table: Ergebnis unabhaengig von der Reihenfolge')
    # Entkopplung: gleicher Eintrag liefert immer denselben Tastgrad.
    check(m.table_select(900000, 900000, table)[1]
          == m.table_select(900000, 900000, table)[1],
          'Table: duty je Eintrag fest (entkoppelt von min/max)')

    # --- Beide Modi enden bei duty 0 -> unter 680 mV geht es nicht ---
    check(m.continuous_rel_duty(680000, **m.N2P) == 100,
          'Continuous-Boden: 680 mV ist rel_duty 100 (reg_duty 0)')
    check(m.table_select(679000, 679000, table) is None,
          'Table-Boden: < 680 mV hat keinen Eintrag')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
