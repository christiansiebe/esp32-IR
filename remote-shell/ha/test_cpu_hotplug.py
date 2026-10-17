#!/usr/bin/env python3
"""Host-Tests fuer cpu_hotplug.py (reine Logik, kein Board, kein Netz).

Geprueft wird die Entscheidung `plan_online` plus die Sicherheitsregeln. Der
Sysfs-Schreibpfad braucht ein echtes Board und ist durch den Lauf auf dem
Odroid abgedeckt.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cpu_hotplug as ch  # noqa: E402

CHECKS = 0
FAILURES = 0

CPUS6 = [0, 1, 2, 3, 4, 5]


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def main():
    # Stufen greifen bei der jeweiligen Last.
    check(ch.plan_online(0.10, CPUS6, CPUS6) == [0], 'idle -> 1 Kern')
    check(ch.plan_online(0.49, CPUS6, CPUS6) == [0], 'knapp unter 0.5 -> 1')
    check(ch.plan_online(0.50, CPUS6, CPUS6) == [0, 1], '0.5 -> 2 Kerne')
    check(ch.plan_online(1.49, CPUS6, CPUS6) == [0, 1], '1.49 -> 2 Kerne')
    check(ch.plan_online(1.50, CPUS6, CPUS6) == [0, 1, 2, 3], '1.5 -> 4 Kerne')
    check(ch.plan_online(2.99, CPUS6, CPUS6) == [0, 1, 2, 3], '2.99 -> 4')
    check(ch.plan_online(3.00, CPUS6, CPUS6) == [0, 1, 2, 3, 4, 5], '3.0 -> alle')
    check(ch.plan_online(99.0, CPUS6, CPUS6) == CPUS6, 'hohe Last -> alle')

    # cpu0 bleibt IMMER online, auch bei der kleinsten Stufe.
    check(0 in ch.plan_online(0.0, CPUS6, CPUS6), 'cpu0 bleibt online')

    # Nie mehr Kerne als vorhanden, nie weniger als min_online.
    check(ch.plan_online(0.0, CPUS6, CPUS6, min_online=4) == [0, 1, 2, 3],
          'min_online wird eingehalten')
    check(len(ch.plan_online(99.0, CPUS6, [0, 1])) == 2, 'nie mehr als vorhanden')

    # Eigene Stufen (Testbarkeit der Schwellen).
    steps = [(1.0, 1), (2.0, 2)]
    check(ch.plan_online(0.5, CPUS6, CPUS6, steps=steps, min_online=1) == [0],
          'eigene Stufen: 1 Kern')
    check(ch.plan_online(1.5, CPUS6, CPUS6, steps=steps, min_online=1) == [0, 1],
          'eigene Stufen: 2 Kerne')

    # Monotonie: mehr Last -> nie weniger Kerne.
    prev = 0
    for i in range(0, 120):
        n = len(ch.plan_online(i / 10.0, CPUS6, CPUS6))
        check(n >= prev, 'monoton bei Last %.1f' % (i / 10.0))
        prev = n

    # Rueckgabe ist sortiert und ohne Duplikate.
    got = ch.plan_online(0.0, CPUS6, CPUS6)
    check(got == sorted(set(got)), 'sortiert und eindeutig')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
