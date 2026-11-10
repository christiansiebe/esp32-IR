#!/usr/bin/env python3
"""Host-Tests fuer die pstore-Spurensuche in boot_journal.py.

Prueft, dass nur FRISCHE Panic-Dumps zaehlen (Altersgrenze), dass beide
pstore-Orte gelesen werden und dass harmlose Dumps ignoriert werden.
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import boot_journal as bj  # noqa: E402

CHECKS = 0
FAILURES = 0

PANIC = "<0>[   50.049214] Kernel panic - not syncing: sysrq triggered crash\n"
CLEAN = "<6>[    0.000000] Booting Linux on physical CPU 0x0\n"


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def write(path, text, age_s=0.0):
    with open(path, 'w') as fh:
        fh.write(text)
    now = time.time()
    os.utime(path, (now - age_s, now - age_s))


def main():
    with tempfile.TemporaryDirectory() as d:
        # 1. Frischer Panic-Dump -> gefunden.
        p = os.path.join(d, 'dmesg-ramoops-0')
        write(p, PANIC, age_s=5)
        got = bj.pstore_panic(dirs=[d], max_age=900)
        check(got == "Kernel panic - not syncing: sysrq triggered crash",
              'frischer Panic-Dump gefunden')

        # 2. Alter Panic-Dump -> ignoriert (kein Fehl-Alarm).
        write(p, PANIC, age_s=100000)
        check(bj.pstore_panic(dirs=[d], max_age=900) is None,
              'alter Panic-Dump wird ignoriert')

        # 3. Harmloser Dump ohne Panic -> None.
        write(p, CLEAN, age_s=5)
        check(bj.pstore_panic(dirs=[d], max_age=900) is None,
              'Dump ohne Panic -> None')

        # 4. Leeres/nicht vorhandenes Verzeichnis -> None, kein Absturz.
        check(bj.pstore_panic(dirs=[os.path.join(d, 'gibtsnicht')]) is None,
              'fehlendes Verzeichnis -> None')

        # 5. Zwei Orte: der frische Panic gewinnt.
        d2 = os.path.join(d, 'archive')
        os.makedirs(d2)
        write(os.path.join(d2, 'dmesg.txt'), CLEAN, age_s=1)
        write(p, PANIC, age_s=5)
        got = bj.pstore_panic(dirs=[d, d2], max_age=900)
        check(got is not None and got.startswith('Kernel panic'),
              'frischer Panic schlaegt harmloses Archiv')

        # 6. Genau an der Grenze: juenger als max_age zaehlt.
        write(p, PANIC, age_s=100)
        check(bj.pstore_panic(dirs=[d], max_age=900) is not None,
              '100s alt < 900s -> zaehlt')
        check(bj.pstore_panic(dirs=[d], max_age=50) is None,
              '100s alt > 50s -> ignoriert')

        # 7. Dedupe: eine bereits verbrauchte mtime wird nicht erneut gemeldet.
        write(p, PANIC, age_s=5)
        mt, txt = bj.find_pstore_panic(dirs=[d], max_age=900)
        check(txt is not None, 'Dump liefert mtime+Text')
        check(bj.find_pstore_panic(dirs=[d], max_age=900,
                                   exclude_mtime=mt) == (None, None),
              'verbrauchter Dump wird nicht erneut gemeldet')
        # Ein frischerer Dump wird trotz altem exclude wieder gefunden.
        p2 = os.path.join(d, 'dmesg-ramoops-1')
        write(p2, PANIC, age_s=1)
        mt2, txt2 = bj.find_pstore_panic(dirs=[d], max_age=900, exclude_mtime=mt)
        check(txt2 is not None and mt2 > mt,
              'neuer Dump trotz exclude gefunden')

        # 8. Monotonie: liegt ein AELTERER Dump neben einem verbrauchten, darf
        #    keiner der beiden erneut gemeldet werden (Board-Bug vom 2026-10-17:
        #    systemd-pstore archiviert mehrere Dumps, der aeltere wurde erneut
        #    gefunden und ueberschrieb die neuere Marke).
        old = os.path.join(d, 'dmesg.txt')          # aelterer Archiv-Dump
        write(old, PANIC, age_s=300)
        newest = os.path.join(d, 'dmesg-ramoops-0')
        write(newest, PANIC, age_s=10)
        mt_new, _ = bj.find_pstore_panic(dirs=[d], max_age=900)
        check(mt_new is not None, 'neuester Dump gefunden')
        check(bj.find_pstore_panic(dirs=[d], max_age=900,
                                   exclude_mtime=mt_new) == (None, None),
              'nach Verbrauch wird KEIN aelterer Dump erneut gemeldet')

    print('test_boot_journal_pstore: %d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
