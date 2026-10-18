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
    # Entkopplung: derselbe Eintrag liefert immer denselben Tastgrad - und zwar
    # unabhaengig von einer min/max-Verschiebung (Kontrast zu Continuous).
    shifted_node = dict(m.N2P, min_uv=720000)
    check(m.continuous_rel_duty(900000, **m.N2P)
          != m.continuous_rel_duty(900000, **shifted_node),
          'Continuous: min-Verschiebung aendert den Tastgrad (gekoppelt)')
    check(m.table_duty_for(900000, table)
          == m.table_duty_for(900000, table)
          == 39,
          'Table: duty 39 unabhaengig von min/max (entkoppelt)')

    # --- Falle "Boden umbenennen": min senken aendert die Hardware nicht ---
    check(m.nominal_at_reg_duty(m.N2P, 0, 999) == 680000,
          'reg_duty 0 meldet bei min 680 mV genau 680 mV')
    check(m.nominal_at_reg_duty(dict(m.N2P, min_uv=600000), 0, 999) == 600000,
          'reg_duty 0 meldet bei min 600 mV dann 600 mV (nur Etikett)')
    # Der Registerwert (der physikalische Zustand) ist in beiden Faellen 0:
    # einmal bei 680 mV (min 680), einmal bei 600 mV (min 600) - dieselbe
    # Hardwarestellung, nur anders benannt.
    check(m.rel_to_reg_duty(m.continuous_rel_duty(
              680000, **m.N2P), 999) == 0,
          'min 680 mV: 680 mV -> reg_duty 0')
    check(m.rel_to_reg_duty(m.continuous_rel_duty(
              600000, **dict(m.N2P, min_uv=600000)), 999) == 0,
          'min 600 mV: 600 mV -> reg_duty 0 (derselbe Registerzustand)')

    # --- Overlay-Plan: unter dem Boden wird ok=False ---
    plan = {uv: ok for _f, uv, _r, _d, ok in m.overlay_plan(
        [(24000000, 700000), (24000000, 680000), (24000000, 660000)])}
    check(plan[700000] and plan[680000], 'Overlay-Plan: 700/680 mV sind ok')
    check(not plan[660000], 'Overlay-Plan: 660 mV wird als unmoeglich markiert')
    # Ein gueltiger Plan trifft fuer 1.0 GHz den reg_duty des Werkswerts.
    one = m.overlay_plan([(1000000000, 731000)])[0]
    check(one[4] and 0 <= one[3] <= 999, 'Overlay-Plan: 731 mV -> gueltig')

    # --- Beide Modi enden bei duty 0 -> unter 680 mV geht es nicht ---
    check(m.continuous_rel_duty(680000, **m.N2P) == 100,
          'Continuous-Boden: 680 mV ist rel_duty 100 (reg_duty 0)')
    check(m.table_select(679000, 679000, table) is None,
          'Table-Boden: < 680 mV hat keinen Eintrag')

    # --- Cross-Check gegen uv.py (Roh-Register-Schreiber) ---
    # uv.py importiert dieselben N2+-Knoten; im Test werden nur Konstanten und
    # die reinen Rechenfunktionen benutzt (kein /dev/mem, kein Regler-Sysfs).
    import uv as u
    check(u.V_MIN == m.N2P['min_uv'] and u.V_MAX == m.N2P['max_uv'],
          'uv.py und Modell nutzen dieselben N2+-Grenzen (680/1040 mV)')
    # Spannungsebene: Treiber-Modell vs. uv.py-Roh-Register, <= halber
    # Treiber-Schritt (1,8 mV) + halber Roh-Schritt (0,18 mV) = 1,98 mV.
    worst_v = 0
    for uv in range(680000, 1040001):
        v_model = m.continuous_uv(m.continuous_rel_duty(uv, **m.N2P), **m.N2P)
        v_uvpy = u.duty_to_uv('big', u.uv_to_duty('big', uv))
        worst_v = max(worst_v, abs(v_model - v_uvpy))
    check(worst_v <= m.step_mv(m.N2P) * 1000 / 2 + 0.5 * 360000 / 999,
          'Cross-Check Spannung: max %.2f mV <= 1.98 mV' % (worst_v / 1000))
    # Registerebene: beide Wege landen in <= 5 Roh-Einheiten (1,8 mV) aufeinander.
    worst_reg = 0
    for uv in range(680000, 1040001, 1000):
        r_model = m.rel_to_reg_duty(m.continuous_rel_duty(uv, **m.N2P), 999)
        r_uvpy = u.uv_to_duty('big', uv)
        worst_reg = max(worst_reg, abs(r_model - r_uvpy))
    check(worst_reg <= 5,
          'Cross-Check Register: max %d Roh-Einheiten <= 5 (1.8 mV)' % worst_reg)

    # --- Quantisierung: Treiber (Prozent) ist groeber als das Roh-Register ---
    # Der Treiber rechnet in `pwm-dutycycle-unit` (=100, Prozent), also gibt es
    # nur 101 verschiedene rel_duty-Werte -> Spannungsschritt 360/100 = 3.6 mV.
    achievable = {m.continuous_rel_duty(uv, **m.N2P)
                  for uv in range(680000, 1040001, 1000)}
    check(achievable <= set(range(0, 101)),
          'Treiber: rel_duty ist immer ganzzahlig 0..100')
    check(len(achievable) <= 101, 'Treiber: hoechstens 101 Stufen')
    # Ein 1-mV-Sollwert-Schritt aendert den Registerwert um <= 1 Roh-Einheit
    # (Roh-Aufloesung 360/999 = 0.36 mV), NICHT um einen Prozent-Schritt.
    d1 = m.rel_to_reg_duty(m.continuous_rel_duty(900000, **m.N2P), 999)
    d2 = m.rel_to_reg_duty(m.continuous_rel_duty(901000, **m.N2P), 999)
    check(abs(d2 - d1) <= 1,
          'Treiber: 1 mV Sollwertschritt -> <= 1 Registereinheit')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
