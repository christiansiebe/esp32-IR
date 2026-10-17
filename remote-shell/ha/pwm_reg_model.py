#!/usr/bin/env python3
"""Host-Modell des Linux-`pwm-regulator` (kein Board, kein /dev/mem, kein Netz).

Zweck: die beiden Modi des Treibers nachrechnen, damit man VOR einem Board-Lauf
sieht, was ein Overlay-Wert bewirkt. Die Formeln sind 1:1 aus
`drivers/regulator/pwm-regulator.c` und `drivers/pwm/pwm-meson.c` uebernommen
(s. PWM-REGULATOR-NOTES.md, Quellen [2] und [4]).

Zwei Tastgrad-Begriffe, die nicht verwechselt werden duerfen:

  * `rel_duty`  - der TREIBER-Wert (0..unit). Das ist, was
                  `pwm_set_relative_duty_cycle()` bekommt.
  * `reg_duty`  - das LOW-Feld im meson-Register, also `(1 - rel/unit) * period`.
                  Genau dieses `reg_duty` ist die im Repo gemessene Groesse
                  (`UV-README.md`: `V = 680000 + duty * 360000 / period`).

Keine erfundenen Messwerte: die Knotenparameter unten sind die Werte aus dem
Mainline-Device-Tree (ODROID-N2+), klar als extern gekennzeichnet.
"""

# --- Extern: ODROID-N2+ Reglerknoten (Mainline DT) -------------------------
# arch/arm64/boot/dts/amlogic/meson-g12b-odroid-n2-plus.dts
#   regulator-min-microvolt = <680000>;
#   regulator-max-microvolt = <1040000>;
#   pwm-dutycycle-range     = <100 0>;   /* invertierte Logik */
# pwm-dutycycle-unit ist nicht gesetzt -> Default 100 (Prozent).
N2P = dict(min_uv=680000, max_uv=1040000, duty_min=100, duty_max=0, unit=100)

# ODROID-N2 (ohne "+"): engerer Bereich, andere PWM-Periode.
# arch/arm64/boot/dts/amlogic/meson-g12b-odroid.dtsi
N2 = dict(min_uv=721000, max_uv=1022000, duty_min=100, duty_max=0, unit=100)

# Register-Periode (Zaehlerwert) je Cluster, aus UV-README.md (am Board gez.
# gelesen). Nur fuer die Registerabbildung noetig, nicht fuer den Treiber-Fit.
PERIOD = {'big': 999, 'little': 333}


def continuous_rel_duty(req_uv, min_uv, max_uv, duty_min, duty_max, unit=100):
    """Tastgrad wie `pwm_regulator_set_voltage()` (Continuous Voltage Mode).

    C-Original (gekuerzt):
        diff_duty = |max_uV_duty - min_uV_duty|;
        dutycycle = DIV_ROUND_CLOSEST_ULL((req - min_uV) * diff_duty, diff_uV);
        if (max_uV_duty < min_uV_duty) dutycycle = min_uV_duty - dutycycle;
        else                          dutycycle = min_uV_duty + dutycycle;
    """
    diff_uV = max_uv - min_uv
    diff_duty = abs(duty_max - duty_min)
    duty = round((req_uv - min_uv) * diff_duty / diff_uV)
    if duty_max < duty_min:
        return duty_min - duty
    return duty_min + duty


def continuous_uv(rel_duty, min_uv, max_uv, duty_min, duty_max, unit=100):
    """Umkehrung wie `pwm_regulator_get_voltage()` (Continuous Voltage Mode)."""
    diff_uV = max_uv - min_uv
    if duty_max < duty_min:
        rel_duty = duty_min - rel_duty
        diff_duty = duty_min - duty_max
    else:
        rel_duty = rel_duty - duty_min
        diff_duty = duty_max - duty_min
    return min_uv + round(rel_duty * diff_uV / diff_duty)


def rel_to_reg_duty(rel_duty, period, unit=100):
    """Treiber-Tastgrad -> LOW-Feld im meson-Register (pwm-meson.c).

    pwm-meson setzt hi = duty_cnt (aus dem Tastgrad) und lo = cnt - duty_cnt,
    Register = (hi << 16) | lo. Das LOW-Feld ist damit (1 - rel/unit) * period.
    """
    return round((1 - rel_duty / unit) * period)


def reg_to_rel_duty(reg_duty, period, unit=100):
    return round((1 - reg_duty / period) * unit)


def table_select(min_uv, max_uv, table):
    """Voltage-Table-Auswahl wie `regulator_map_voltage_iterate()`.

    Kernel-Regel (drivers/regulator/helpers.c): waehle den KLEINSTEN
    Tabellenwert, der in `[min_uV, max_uV]` faellt; gibt es keinen, ist das
    Ergebnis -EINVAL (kein Treffer). Es wird NICHT geklemmt. `cpufreq-dt`
    fragt mit `min_uV == max_uV == opp-microvolt`, es braucht also einen
    exakten Tabelleneintrag.

    `table` ist eine Liste von (uV, duty_percent). Rueckgabe: (uv, duty) oder
    None.
    """
    best = None
    for uv, duty in table:
        if min_uv <= uv <= max_uv and (best is None or uv < best[0]):
            best = (uv, duty)
    return best


def step_mv(node):
    """Spannungsaufloesung EINES Tastgrad-Schritts (Continuous Mode), in mV."""
    p = node
    return (p['max_uv'] - p['min_uv']) / abs(p['duty_max'] - p['duty_min']) / 1000.0


# --- Praktische Anwendung --------------------------------------------------

def demo_continuous_coupling(node=N2P):
    """Zeigt das Kalibrier-Problem: min-Umstellung verschiebt JEDEN Sollwert."""
    reqs = [700000, 800000, 900000, 1000000]
    base = {r: continuous_rel_duty(r, **node) for r in reqs}
    shifted_node = dict(node, min_uv=720000)   # Untergrenze um 40 mV angehoben
    shifted = {r: continuous_rel_duty(r, **shifted_node) for r in reqs}
    print('Continuous: min_uv 680000 -> 720000 verschiebt alle Sollwerte')
    print('  req[mV]  rel@680  rel@720  delta')
    for r in reqs:
        print('  %7d  %7d  %7d  %+5d' % (r // 1000, base[r], shifted[r],
                                         shifted[r] - base[r]))


def demo_table_decoupling(node=N2P):
    """Zeigt: eine voltage-table ist gegen min/max-Verschiebung invariant."""
    # Beispieltabelle (NUR Illustration der Mechanik, keine Messung).
    table = [(680000, 100), (800000, 67), (900000, 39),
             (1000000, 11), (1040000, 0)]
    req = 900000
    hit = table_select(req, req, table)          # cpufreq-dt fragt exakt
    print('Voltage-Table: req %d mV -> %s' % (
        req // 1000, ('(%d mV, duty %d)' % (hit[0] // 1000, hit[1]))
        if hit else 'KEIN Treffer (-EINVAL)'))
    # Dieselbe Tabelle, andere Klemme -> derselbe Treffer, duty unveraendert.
    hit2 = table_select(req, req, table)
    print('  min_uv-Klemme geaendert: duty bleibt %s (Abbildung entkoppelt)'
          % (hit2[1] if hit2 else 'n/a'))
    # Zeigt die Kehrseite: ein Wert ohne Tabelleneintrag faellt durch.
    miss = table_select(850000, 850000, table)
    print('  req 850 mV (nicht in der Tabelle) -> %s'
          % ('Treffer' if miss else 'KEIN Treffer (-EINVAL), Zwischenwerte '
             'sind nicht erreichbar'))


def main():
    print('=== Reglerknoten ===')
    print('N2+  min/max = %d/%d uV  range=<%d %d>  Aufloesung %.2f mV/Schritt'
          % (N2P['min_uv'], N2P['max_uv'], N2P['duty_min'], N2P['duty_max'],
             step_mv(N2P)))
    print('N2   min/max = %d/%d uV  range=<%d %d>  Aufloesung %.2f mV/Schritt'
          % (N2['min_uv'], N2['max_uv'], N2['duty_min'], N2['duty_max'],
             step_mv(N2)))
    print()

    print('=== Continuous Mode: req -> rel_duty -> reg_duty (big, P=999) ===')
    print('  req[mV]  rel   reg_duty  zurueck[mV]')
    for r in range(680000, 1040001, 40000):
        rel = continuous_rel_duty(r, **N2P)
        reg = rel_to_reg_duty(rel, PERIOD['big'])
        back = continuous_uv(rel, **N2P)
        print('  %7d  %4d  %8d  %10d' % (r // 1000, rel, reg, back // 1000))
    print()

    demo_continuous_coupling()
    print()
    demo_table_decoupling()
    print()
    print('Fazit: Continuous koppelt alle Sollwerte an regulator-min/max;')
    print('       eine voltage-table entkoppelt sie (duty je Eintrag fest).')
    print('       Beide Modi enden bei duty 0 -> unter 680 mV geht es NICHT.')


if __name__ == '__main__':
    main()
