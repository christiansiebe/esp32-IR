#!/usr/bin/env python3
"""End-to-End-Host-Test des boot_journal-Ablaufs (ohne Board).

Simuliert die Folge: sauberer Reboot -> Panic -> Panic-Reboot -> sauberer
Reboot. Geprueft wird, dass
  * der Panic-Boot den Grund `kernel-panic` MIT Text bekommt,
  * der Panic-Text NICHT an den folgenden (sauberen) Boot "klebt",
  * ein alter pstore-Dump keinem spaeteren Boot erneut zugeordnet wird.

Das Board selbst liefert /proc/uptime, watchdog, boot_id und journalctl; hier
werden diese Spuren per Umgebung ersetzt, sodass nur die echte Logik laeuft.
"""
import importlib
import json
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

CHECKS = 0
FAILURES = 0

PANIC = "<0>[   50.049214] Kernel panic - not syncing: sysrq triggered crash\n"


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def fresh_module(tmp, pstore_dir, uptime):
    """Laedt boot_journal neu mit ersetzten Board-Quellen."""
    os.environ['BOOT_JOURNAL_DIR'] = tmp
    os.environ['BOOT_JOURNAL_PSTORE_DIRS'] = pstore_dir
    for m in ('boot_journal', 'boot_journal_lib'):
        sys.modules.pop(m, None)
    bj = importlib.import_module('boot_journal')
    # Board-Quellen ersetzen: fester boot_id, feste Uptime, kein Journal.
    bj.current_boot_id = lambda: 'boot-%s' % uptime
    bj.uptime_s = lambda: uptime
    bj.read_watchdog_bootstatus = lambda: False
    bj.prev_kernel_panic = lambda: False
    bj.kernel_release = lambda: 'test-kernel'
    return bj


def state_of(tmp):
    with open(os.path.join(tmp, 'state.json')) as fh:
        return json.load(fh)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        pstore = os.path.join(tmp, 'pstore')
        os.makedirs(pstore)

        # --- Boot 1: first-boot ---
        bj = fresh_module(tmp, pstore, 5.0)
        bj.cmd_start(None)
        st = state_of(tmp)
        check(st['boots_total'] == 1 and st['last']['reason'] == 'first-boot',
              'Boot 1 = first-boot')

        # Heartbeat auf 300s, dann sauberer Reboot (Marker).
        os.environ['BOOT_JOURNAL_DIR'] = tmp
        bj.cmd_heartbeat(None)
        bj.cmd_mark_clean(None)

        # --- Boot 2: clean-reboot (kein Panic) ---
        bj = fresh_module(tmp, pstore, 3.0)
        bj.cmd_start(None)
        st = state_of(tmp)
        check(st['last']['reason'] == 'clean-reboot', 'Boot 2 = clean-reboot')
        check('panic' not in st['last'], 'sauberer Boot ohne Panic-Feld')

        # --- Panic! frischer pstore-Dump, kein Clean-Marker ---
        p = os.path.join(pstore, 'dmesg-ramoops-0')
        with open(p, 'w') as fh:
            fh.write(PANIC)

        # --- Boot 3: kernel-panic ---
        bj = fresh_module(tmp, pstore, 20.0)
        bj.cmd_start(None)
        st = state_of(tmp)
        check(st['last']['reason'] == 'kernel-panic', 'Boot 3 = kernel-panic')
        check(st['last'].get('panic', '').startswith('Kernel panic'),
              'Boot 3 traegt Panic-Text')
        check('consumed_pstore_mtime' in st, 'verbrauchte mtime gespeichert')

        # --- Boot 4: sauberer Reboot; der ALTE Dump darf nicht erneut zaehlen ---
        bj.cmd_mark_clean(None)
        bj = fresh_module(tmp, pstore, 500.0)
        bj.cmd_start(None)
        st = state_of(tmp)
        check(st['last']['reason'] == 'clean-reboot',
              'Boot 4 = clean-reboot (alter Dump ignoriert)')
        check('panic' not in st['last'],
              'Boot 4 traegt KEINEN Panic-Text (Dedupe greift)')

        # --- Boot 5: Hard-Reset (kein Marker), Dump noch alt -> kein Panic ---
        bj = fresh_module(tmp, pstore, 400.0)
        bj.cmd_start(None)
        st = state_of(tmp)
        check(st['last']['reason'] == 'hard-reset-unknown',
              'Boot 5 = hard-reset-unknown (alter Dump zaehlt nicht)')
        check('panic' not in st['last'],
              'Boot 5 traegt KEINEN Panic-Text')

    print('test_boot_journal_flow: %d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
