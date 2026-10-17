#!/usr/bin/env python3
"""Host-Tests fuer die htopv-Werkzeuge (cpu-volt-watch, cpu-volt-line).

Kein Board, kein Netz: die Skripte lesen ihr sysfs ueber die Umgebungsvariablen
CPU_SYS / REG_SYS / THERMAL_SYS. Der Test baut einen Fake-Baum, fuehrt die
Skripte als echte Subprozesse aus und prueft die Ausgabe.

Der wichtigste Punkt ist die **Rail-Zuordnung**: VDDCPU_A = big (policy2),
VDDCPU_B = little (policy0). Auf dem Board empirisch bestaetigt (nur policy0 auf
MAX -> VDDCPU_B steigt; nur policy2 auf MAX -> VDDCPU_A steigt).
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
WATCH = os.path.join(HERE, 'cpu-volt-watch')
LINE = os.path.join(HERE, 'cpu-volt-line')

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def write(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as fh:
        fh.write(str(value) + '\n')


def build_fake_sysfs(root, little_mhz, big_mhz, a_uv, b_uv):
    cpu = os.path.join(root, 'cpu')
    reg = os.path.join(root, 'regulator')
    therm = os.path.join(root, 'thermal')

    for c in range(6):
        write(os.path.join(cpu, 'cpu%d' % c, 'online'), 1)
    write(os.path.join(cpu, 'cpufreq', 'policy0', 'related_cpus'), '0 1')
    write(os.path.join(cpu, 'cpufreq', 'policy2', 'related_cpus'), '2 3 4 5')
    write(os.path.join(cpu, 'cpufreq', 'policy0', 'scaling_cur_freq'), little_mhz * 1000)
    write(os.path.join(cpu, 'cpufreq', 'policy2', 'scaling_cur_freq'), big_mhz * 1000)

    # Regulator-Namen bewusst in "falscher" Reihenfolge, damit der Test die
    # Namenssuche und nicht die Verzeichnisnummer prueft.
    write(os.path.join(reg, 'regulator.2', 'name'), 'VDDCPU_A')
    write(os.path.join(reg, 'regulator.2', 'microvolts'), a_uv)
    write(os.path.join(reg, 'regulator.8', 'name'), 'VDDCPU_B')
    write(os.path.join(reg, 'regulator.8', 'microvolts'), b_uv)

    write(os.path.join(therm, 'thermal_zone0', 'type'), 'cpu-thermal')
    write(os.path.join(therm, 'thermal_zone0', 'temp'), 42000)
    return cpu, reg, therm


def run(script, cpu, reg, therm):
    env = dict(os.environ, CPU_SYS=cpu, REG_SYS=reg, THERMAL_SYS=therm)
    return subprocess.run(['/bin/sh', script], env=env, capture_output=True,
                          text=True, check=True).stdout


def main():
    with tempfile.TemporaryDirectory() as root:
        # little 1000 MHz / 0.763 V, big 1704 MHz / 0.730 V (reale Board-Werte).
        cpu, reg, therm = build_fake_sysfs(root, 1000, 1704, 730400, 762800)

        out = run(WATCH, cpu, reg, therm)
        check('little' in out, 'watch: little-Cluster fehlt')
        check('big' in out, 'watch: big-Cluster fehlt')
        check('1000' in out, 'watch: little-MHz fehlt')
        check('1704' in out, 'watch: big-MHz fehlt')
        check('0.730' in out, 'watch: VDDCPU_A (big) Volt fehlt')
        check('0.763' in out, 'watch: VDDCPU_B (little) Volt fehlt')
        check('cpu-thermal' in out, 'watch: Temperaturzone fehlt')
        check('42.0' in out, 'watch: Temperaturwert fehlt')
        # cpu0-1 little, cpu2-5 big (related_cpus).
        check('cpu0,1' in out, 'watch: little related_cpus fehlt')
        check('cpu2,3,4,5' in out, 'watch: big related_cpus fehlt')

        line = run(LINE, cpu, reg, therm)
        # little nutzt VDDCPU_B, big nutzt VDDCPU_A -> die Zuordnung muss stimmen.
        check('little 1000MHz/0.763V' in line,
              'line: little muss VDDCPU_B (0.763) nutzen, war: %r' % line)
        check('big 1704MHz/0.730V' in line,
              'line: big muss VDDCPU_A (0.730) nutzen, war: %r' % line)
        check('cpu 42C' in line, 'line: Temperatur fehlt, war: %r' % line)

        # Gegenprobe: Rails vertauscht -> die Ausgabe MUSS sich aendern.
        cpu, reg, therm = build_fake_sysfs(root, 1000, 1704, 762800, 730400)
        swapped = run(LINE, cpu, reg, therm)
        check(swapped != line, 'line: vertauschte Rails werden nicht unterschieden')

        # Offline-Cluster: fehlende scaling_cur_freq -> "-" statt Absturz.
        os.remove(os.path.join(cpu, 'cpufreq', 'policy2', 'scaling_cur_freq'))
        out = run(WATCH, cpu, reg, therm)
        check('-' in out, 'watch: offline-Cluster muss "-" zeigen')

    print('htopv: %d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
