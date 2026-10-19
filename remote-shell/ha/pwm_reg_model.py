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


def nominal_at_reg_duty(node, reg_duty, period):
    """Welche Spannung MELDET der DT fuer einen gegebenen Registerwert?

    Der Registerwert ist der physikalische Zustand. Die Zuordnung Register ->
    Nominalspannung haengt aber an `regulator-min/max-microvolt`. Damit laesst
    sich zeigen, dass ein Absenken von `regulator-min-microvolt` den Boden nur
    UMBENENNT, nicht absenkt.
    """
    rel = reg_to_rel_duty(reg_duty, period, node['unit'])
    return continuous_uv(rel, **node)


def relabel_floor_demo(period=999):
    """Zeigt die Falle: min_uv senken benennt den Boden um, senkt ihn nicht.

    Physikalisch ist `reg_duty == 0` der Boden (kleiner geht das LOW-Feld
    nicht). Welche Spannung dieser Zustand TRAEGT, entscheidet allein die
    DT-Abbildung. Ein Overlay mit kleinerem `regulator-min-microvolt` laesst
    die OPP-Tabelle eine tiefere Spannung behaupten, waehrend die Hardware
    unveraendert am Boden bleibt.
    """
    print('Falle "Boden umbenennen" (big, P=%d):' % period)
    print('  node min/max[mV]   gemeldete Spannung bei reg_duty=0')
    for node in (N2P, dict(N2P, min_uv=600000), dict(N2P, min_uv=720000)):
        print('  %4d/%-4d         %6d mV'
              % (node['min_uv'] // 1000, node['max_uv'] // 1000,
                 nominal_at_reg_duty(node, 0, period) // 1000))
    print('  -> reg_duty=0 ist EIN Hardwarezustand; die gemeldete Zahl ist')
    print('     reine Etikettierung. Tiefer wird die Spannung dadurch nicht.')


def overlay_plan(rates_uv, node=N2P, period=None):
    """Praktisch: Wunschspannungen je Takt auf Registerwerte abbilden.

    `rates_uv` ist eine Liste von (freq_hz, uV). Rueckgabe: Liste von
    (freq_hz, uV, rel_duty, reg_duty, ok). `ok` ist False, wenn der Wert unter
    dem Regler-Boden (duty 0) oder ueber dem Maximum liegt - so sieht man VOR
    dem Overlay-Bau, was gar nicht geht.
    """
    if period is None:
        period = PERIOD['big']
    out = []
    for freq, uv in rates_uv:
        rel = continuous_rel_duty(uv, **node)
        reg = rel_to_reg_duty(rel, period, node['unit'])
        ok = (node['min_uv'] <= uv <= node['max_uv']
              and 0 <= rel <= node['unit'] and 0 <= reg <= period)
        out.append((freq, uv, rel, reg, ok))
    return out


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


def table_duty_for(req_uv, table):
    """Duty, den die `voltage-table` fuer einen exakten Sollwert liefert.

    Anders als im Continuous-Modus haengt dieser Wert NUR an der Tabelle, nicht
    an `regulator-min/max-microvolt` (die wirken hier nur als Klemme). Genau das
    ist die "Entkopplung".
    """
    hit = table_select(req_uv, req_uv, table)
    return None if hit is None else hit[1]


def demo_table_decoupling(node=N2P):
    """Zeigt: eine voltage-table ist gegen min/max-Verschiebung invariant.

    Kontrast: im Continuous-Modus verschiebt dieselbe min-Aenderung den
    Tastgrad, in der Tabelle bleibt er gleich.
    """
    # Beispieltabelle (NUR Illustration der Mechanik, keine Messung).
    table = [(680000, 100), (800000, 67), (900000, 39),
             (1000000, 11), (1040000, 0)]
    req = 900000
    shifted_node = dict(node, min_uv=720000)

    cont_a = continuous_rel_duty(req, **node)
    cont_b = continuous_rel_duty(req, **shifted_node)
    tab = table_duty_for(req, table)

    hit = table_select(req, req, table)          # cpufreq-dt fragt exakt
    print('Voltage-Table: req %d mV -> %s' % (
        req // 1000, ('(%d mV, duty %d)' % (hit[0] // 1000, hit[1]))
        if hit else 'KEIN Treffer (-EINVAL)'))
    # Continuous koppelt: dieselbe min-Verschiebung aendert den Tastgrad.
    print('  Continuous-Kontrast: rel_duty %d (@min %d mV) -> %d (@min %d mV)'
          % (cont_a, node['min_uv'] // 1000, cont_b, shifted_node['min_uv'] // 1000))
    # Tabelle entkoppelt: derselbe Eintrag, derselbe duty, unabhaengig von min.
    print('  Tabelle (entkoppelt): duty bleibt %s bei min %d wie bei min %d'
          % (tab, node['min_uv'] // 1000, shifted_node['min_uv'] // 1000))
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
    relabel_floor_demo()
    print()

    print('=== Overlay-Plan: Wunschspannung je Takt -> Registerwert ===')
    print('  freq[MHz]  uV      rel   reg_duty  ok')
    plan = overlay_plan([(1000000000, 731000), (24000000, 690000),
                         (24000000, 660000)])
    for freq, uv, rel, reg, ok in plan:
        print('  %9d  %6d  %4d  %8d  %s'
              % (freq // 1000, uv, rel, reg, 'ja' if ok else 'NEIN (<Boden)'))
    print()
    print('Fazit: Continuous koppelt alle Sollwerte an regulator-min/max;')
    print('       eine voltage-table entkoppelt sie (duty je Eintrag fest).')
    print('       Beide Modi enden bei duty 0 -> unter 680 mV geht es NICHT;')
    print('       ein kleineres regulator-min-microvolt benennt den Boden nur um.')


if __name__ == '__main__':
    main()
