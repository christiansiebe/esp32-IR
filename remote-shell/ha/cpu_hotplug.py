#!/usr/bin/env python3
"""CPU-Hotplug-Governor fuer den Odroid N2+.

Der Kernel hat keinen cpuidle-Treiber (`current_driver` = `none`, das DTB
enthaelt keine `idle-states`), die Kerne gehen im Leerlauf also **nicht** in
einen Schlafzustand. Der einzige Weg, einen Kern wirklich stillzulegen, ist
Hotplug: `/sys/devices/system/cpu/cpuN/online` auf 0. Ein offline genommener
Kern verbraucht keinen Takt und keine Leckleistung mehr.

Dieser Daemon beobachtet die Last und nimmt ungenutzte Kerne offline bzw.
holt sie bei Bedarf zurueck. Die Entscheidung selbst ist eine reine Funktion
(`plan_online`) und wird ohne Board getestet (test_cpu_hotplug.py).

Sicherheitsregeln:
- cpu0 bleibt **immer** online. Der Boot-Prozessor ist der einzige, dessen
  Wieder-Onlinen nicht garantiert ist; ausserdem haengen Timer an ihm.
- Es bleiben mindestens `MIN_ONLINE` Kerne online.
- Hysterese: eine Aenderung passiert erst, wenn die Last `STABLE_SAMPLES`
  Messungen in Folge dieselbe Stufe verlangt. Das verhindert Flattern.
- Beim Beenden (Signal) werden **alle** Kerne wieder online genommen.
"""

import os
import signal
import sys
import time

CPU_SYS = '/sys/devices/system/cpu'

# (Last-Schwelle, gewuenschte Kernzahl). Die erste passende Zeile gewinnt.
# Aufwaermend sortiert; die letzte Stufe ist die Gesamtzahl der Kerne.
STEPS = [
    (0.50, 2),
    (1.50, 3),
    (3.00, 4),
    (6.00, 6),
]
MIN_ONLINE = 2
STABLE_SAMPLES = 6      # 6 x INTERVAL = 30 s stabile Last vor einer Aenderung
INTERVAL = 5.0


def all_cpus():
    out = []
    for name in os.listdir(CPU_SYS):
        if name.startswith('cpu') and name[3:].isdigit():
            out.append(int(name[3:]))
    return sorted(out)


def online_cpus():
    out = []
    for c in all_cpus():
        try:
            with open('%s/cpu%d/online' % (CPU_SYS, c)) as fh:
                if fh.read().strip() == '1':
                    out.append(c)
        except OSError:
            out.append(c)          # cpu0 hat kein online-File -> immer online
    return sorted(out)


def plan_online(load1, online, cpus, steps=None, min_online=MIN_ONLINE):
    """Reine Entscheidung: welche Kerne sollen online sein?

    load1  - 1-Minuten-Last
    online - aktuell online genommene Kern-IDs
    cpus   - alle vorhandenen Kern-IDs
    Rueckgabe: sortierte Liste der Kern-IDs, die online sein sollen.

    Es wird immer die niedrigste Kern-ID zuerst online gehalten, damit cpu0
    nie abgeschaltet wird. `min_online` wird nie unterschritten und `cpus`
    nie ueberschritten.
    """
    steps = steps if steps is not None else STEPS
    want = cpus[-1] + 1
    for threshold, count in steps:
        if load1 < threshold:
            want = count
            break
    want = max(min_online, min(want, len(cpus)))
    # Niedrigste IDs bevorzugen -> cpu0 bleibt garantiert dabei.
    return sorted(cpus)[:want]


def loadavg():
    with open('/proc/loadavg') as fh:
        return float(fh.read().split()[0])


def apply_plan(target, cpus):
    """Setzt den Plan um. Rueckgabe: (offlined, onlined)."""
    offlined, onlined = [], []
    for c in cpus:
        if c == 0:
            continue                     # cpu0 nie anfassen
        path = '%s/cpu%d/online' % (CPU_SYS, c)
        if not os.path.exists(path):
            continue
        want = '1' if c in target else '0'
        try:
            with open(path) as fh:
                have = fh.read().strip()
            if have == want:
                continue
            with open(path, 'w') as fh:
                fh.write(want)
            (onlined if want == '1' else offlined).append(c)
        except OSError as exc:
            print('cpu%d: %s' % (c, exc), file=sys.stderr)
    return offlined, onlined


def restore_all(cpus):
    for c in cpus:
        path = '%s/cpu%d/online' % (CPU_SYS, c)
        try:
            with open(path, 'w') as fh:
                fh.write('1')
        except OSError:
            pass


def run():
    cpus = all_cpus()
    print('cpu-hotplug: %d Kerne, Stufen %s' % (len(cpus), STEPS), flush=True)

    def on_signal(signum, _frame):
        print('Signal %d -> alle Kerne online' % signum, flush=True)
        restore_all(cpus)
        sys.exit(0)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    pending = None
    count = 0
    while True:
        load1 = loadavg()
        target = plan_online(load1, online_cpus(), cpus)
        if pending == target:
            count += 1
        else:
            pending, count = target, 1
        if count >= STABLE_SAMPLES:
            off, on = apply_plan(target, cpus)
            if off or on:
                print('Last %.2f -> online %s (aus: %s, an: %s)'
                      % (load1, target, off or '-', on or '-'), flush=True)
            count = 0
        time.sleep(INTERVAL)


if __name__ == '__main__':
    run()
