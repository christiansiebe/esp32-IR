#!/usr/bin/env python3
"""Host-Tests fuer cpu-live (Takt/Spannung/Temperatur ohne htop-Last).

Kein Board, kein Netz: cpu-live liest sysfs ueber CPU_SYS / REG_SYS /
THERMAL_SYS. Der Test baut einen Fake-Baum und fuehrt das Skript als echten
Subprozess aus - derselbe Stil wie test_htopv.py.

Geprueft wird: Ausgabeformat, die Rail-Zuordnung (VDDCPU_A=big, VDDCPU_B=little),
robustes Verhalten bei fehlenden/offline-Werten, --once, -n und --csv.
"""
import csv
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LIVE = os.path.join(HERE, "cpu-live")

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def write(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(str(value) + "\n")


def build(root, little=1000, big=1704, a_uv=730400, b_uv=762800, temp=42000):
    cpu = os.path.join(root, "cpu")
    reg = os.path.join(root, "regulator")
    therm = os.path.join(root, "thermal")
    write(os.path.join(cpu, "cpufreq/policy0/scaling_cur_freq"), little * 1000)
    write(os.path.join(cpu, "cpufreq/policy2/scaling_cur_freq"), big * 1000)
    # Namen bewusst in "falscher" Verzeichnis-Reihenfolge (Namenssuche testen).
    write(os.path.join(reg, "regulator.2/name"), "VDDCPU_A")
    write(os.path.join(reg, "regulator.2/microvolts"), a_uv)
    write(os.path.join(reg, "regulator.8/name"), "VDDCPU_B")
    write(os.path.join(reg, "regulator.8/microvolts"), b_uv)
    write(os.path.join(therm, "thermal_zone0/type"), "cpu-thermal")
    write(os.path.join(therm, "thermal_zone0/temp"), temp)
    return cpu, reg, therm


def run(cpu, reg, therm, *args):
    env = dict(os.environ, CPU_SYS=cpu, REG_SYS=reg, THERMAL_SYS=therm)
    return subprocess.run([sys.executable, LIVE, *args], env=env,
                          capture_output=True, text=True, check=True).stdout


def main():
    with tempfile.TemporaryDirectory() as root:
        cpu, reg, therm = build(root)

        out = run(cpu, reg, therm, "--once")
        check("little 1000MHz/0.763V" in out,
              "little muss VDDCPU_B (0.763) nutzen, war: %r" % out)
        check("big 1704MHz/0.730V" in out,
              "big muss VDDCPU_A (0.730) nutzen, war: %r" % out)
        check("cpu 42.0C" in out, "Temperatur fehlt, war: %r" % out)

        # Gegenprobe: Rails vertauscht -> Ausgabe MUSS sich aendern.
        cpu, reg, therm = build(root, a_uv=762800, b_uv=730400)
        swapped = run(cpu, reg, therm, "--once")
        check(swapped != out, "vertauschte Rails werden nicht unterschieden")

        # Mehrere Werte, nicht-live -> je eine ganze Zeile.
        cpu, reg, therm = build(root)
        lines = run(cpu, reg, therm, "-n", "3", "-i", "0").strip().splitlines()
        check(len(lines) == 3, "3 Messwerte erwartet, waren %d" % len(lines))

        # CSV: Kopfzeile + n Datenzeilen, Spalten in fester Reihenfolge.
        csv_path = os.path.join(root, "out.csv")
        run(cpu, reg, therm, "-n", "2", "-i", "0", "--csv", csv_path)
        with open(csv_path) as fh:
            rows = list(csv.reader(fh))
        check(rows[0] == ["epoch", "little_mhz", "big_mhz",
                          "little_uv", "big_uv", "temp_c"],
              "CSV-Kopfzeile falsch: %r" % rows[0])
        check(len(rows) == 3, "CSV: 1 Kopf + 2 Zeilen erwartet, waren %d" % len(rows))
        check(rows[1][1:3] == ["1000", "1704"], "CSV-MHz falsch: %r" % rows[1])

        # Anhaengen: zweiter Lauf schreibt KEINEN neuen Kopf.
        run(cpu, reg, therm, "-n", "1", "-i", "0", "--csv", csv_path)
        with open(csv_path) as fh:
            rows2 = list(csv.reader(fh))
        check(len(rows2) == 4, "CSV-Anhaengen: 4 Zeilen erwartet, waren %d" % len(rows2))
        check(rows2[0][0] == "epoch", "Kopfzeile wurde erneut geschrieben")

        # Offline-Cluster (fehlende Datei) -> "-", kein Absturz.
        os.remove(os.path.join(cpu, "cpufreq/policy2/scaling_cur_freq"))
        os.remove(os.path.join(reg, "regulator.2/microvolts"))
        out = run(cpu, reg, therm, "--once")
        check("big -MHz/-V" in out, "offline big muss '-MHz/-V' zeigen: %r" % out)
        check("little 1000MHz/0.763V" in out,
              "little muss trotz offline-big weiter messen: %r" % out)

        # Fehlender Regler komplett (kein regulator-Verzeichnis) -> kein Absturz.
        cpu, reg, therm = build(root)
        os.rename(reg, reg + ".hidden")
        out = run(cpu, reg, therm, "--once")
        check("little 1000MHz/-V" in out,
              "fehlender Reglerbaum -> '-V', war: %r" % out)

    print("cpu-live: %d Checks, %d Fehler" % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
