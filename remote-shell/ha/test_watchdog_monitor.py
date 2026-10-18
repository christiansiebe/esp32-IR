#!/usr/bin/env python3
"""Host-Tests fuer watchdog_monitor.py (reine Logik, kein Board, kein Netz).

Geprueft wird die Entscheidungslogik `decide` / `parse_heartbeat` / `is_stale`
plus das Zustandsfortschreiben in `run_once` (nur Datei lesen, kein Aktor).
Der reale Aktor (Power-Cut) braucht Hardware und ist hier absichtlich nicht
Teil des Tests.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import watchdog_monitor as wm  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def main():
    # --- parse_heartbeat -------------------------------------------------
    check(wm.parse_heartbeat('1234 bootA 5\n') == {'boot_id': 'bootA', 'seq': 5},
          '3 Felder: epoch wird verworfen')
    check(wm.parse_heartbeat('bootA 5\n') == {'boot_id': 'bootA', 'seq': 5},
          '2 Felder: boot_id + seq')
    check(wm.parse_heartbeat('') is None, 'leer -> None')
    check(wm.parse_heartbeat('   \n\n') is None, 'nur Whitespace -> None')
    check(wm.parse_heartbeat('bootA\n') is None, '1 Feld -> None')
    check(wm.parse_heartbeat('bootA xx\n') is None, 'seq keine Zahl -> None')
    # Mehrere Zeilen: die LETZTE (neueste) zaehlt.
    check(wm.parse_heartbeat('a 1\nb 2\n') == {'boot_id': 'b', 'seq': 2},
          'mehrzeilig -> letzte Zeile')

    # --- is_stale --------------------------------------------------------
    check(wm.is_stale(100, None, 60, 30) is True, 'kein last_seen -> stale')
    check(wm.is_stale(100, 100, 60, 30) is False, 'gerade gesehen -> frisch')
    check(wm.is_stale(190, 100, 60, 30) is False, '90 == Grenze -> nicht stale')
    check(wm.is_stale(191, 100, 60, 30) is True, '91 > Grenze -> stale')

    # --- decide: frischer Heartbeat -------------------------------------
    d = wm.decide(now=100, last_seen=100, boot_id='A', last_boot_id='A',
                  miss_count=5, interval=60, grace=30, miss_needed=3,
                  cooldown=600, last_reboot=None)
    check(d['action'] == 'ok' and d['miss_count'] == 0,
          'frisch -> ok und Zaehler zurueckgesetzt')

    # --- decide: erste Fehlversuche -------------------------------------
    d = wm.decide(now=200, last_seen=100, boot_id='A', last_boot_id='A',
                  miss_count=0, interval=60, grace=30, miss_needed=3,
                  cooldown=600, last_reboot=None)
    check(d['action'] == 'miss' and d['miss_count'] == 1, '1. Fehlversuch -> miss')
    d = wm.decide(now=200, last_seen=100, boot_id='A', last_boot_id='A',
                  miss_count=1, interval=60, grace=30, miss_needed=3,
                  cooldown=600, last_reboot=None)
    check(d['action'] == 'miss' and d['miss_count'] == 2, '2. Fehlversuch -> miss')

    # --- decide: Reboot-Schwelle ----------------------------------------
    d = wm.decide(now=200, last_seen=100, boot_id='A', last_boot_id='A',
                  miss_count=2, interval=60, grace=30, miss_needed=3,
                  cooldown=600, last_reboot=None)
    check(d['action'] == 'reboot' and d['miss_count'] == 3,
          '3. Fehlversuch -> reboot')

    # --- decide: Cooldown verhindert Reboot-Sturm -----------------------
    d = wm.decide(now=200, last_seen=100, boot_id='A', last_boot_id='A',
                  miss_count=2, interval=60, grace=30, miss_needed=3,
                  cooldown=600, last_reboot=150)
    check(d['action'] == 'cooldown', 'Cooldown aktiv -> kein Reboot')
    d = wm.decide(now=800, last_seen=100, boot_id='A', last_boot_id='A',
                  miss_count=2, interval=60, grace=30, miss_needed=3,
                  cooldown=600, last_reboot=150)
    check(d['action'] == 'reboot', 'Cooldown abgelaufen -> Reboot')

    # --- decide: neuer boot_id = Board hat selbst rebootet ---------------
    d = wm.decide(now=200, last_seen=100, boot_id='B', last_boot_id='A',
                  miss_count=2, interval=60, grace=30, miss_needed=3,
                  cooldown=600, last_reboot=None)
    check(d['action'] == 'ok', 'boot-id wechselt -> ok (kein Reboot)')

    # --- decide: Defaults greifen, wenn None uebergeben ------------------
    d = wm.decide(now=100, last_seen=100, boot_id=None, last_boot_id=None,
                  miss_count=0)
    check(d['action'] == 'ok', 'Defaults + frisch -> ok')

    # --- run_once: Zustandsmaschine ueber eine simulierte Zeitachse ------
    # Die Datei aendert sich nur, wenn der Emitter schreibt. Zwischen den
    # Schreibvorgaengen steht der Inhalt -> die Alter zaehlt ab der letzten
    # Inhaltsaenderung.
    with tempfile.TemporaryDirectory() as td:
        hbpath = os.path.join(td, 'hb')
        with open(hbpath, 'w') as fh:
            fh.write('1000 bootA 1\n')
        cfg = {'interval': 60, 'grace': 30, 'miss_needed': 3, 'cooldown': 600}
        state = {}

        state, d = wm.run_once(state, hbpath, 1000, cfg)
        check(d['action'] == 'ok', 'run_once: frischer Heartbeat -> ok')
        check(state['last_change'] == 1000, 'run_once: last_change gestempelt')

        # Kein neuer Heartbeat -> Fehlversuche bis Reboot.
        state, d = wm.run_once(state, hbpath, 1100, cfg)
        check(d['action'] == 'miss' and state['miss_count'] == 1,
              'run_once: 1. miss')
        state, d = wm.run_once(state, hbpath, 1110, cfg)
        check(d['action'] == 'miss' and state['miss_count'] == 2,
              'run_once: 2. miss')
        state, d = wm.run_once(state, hbpath, 1120, cfg)
        check(d['action'] == 'reboot', 'run_once: reboot nach 3 Fehlversuchen')
        check(state['last_reboot'] == 1120, 'run_once: Reboot-Zeit gemerkt')

        # Direkt danach: Cooldown blockt einen zweiten Reboot.
        state, d = wm.run_once(state, hbpath, 1130, cfg)
        check(d['action'] == 'cooldown', 'run_once: Cooldown blockt')

        # Neuer Boot schreibt neuen Heartbeat -> sofort ok.
        with open(hbpath, 'w') as fh:
            fh.write('1200 bootB 1\n')
        state, d = wm.run_once(state, hbpath, 1200, cfg)
        check(d['action'] == 'ok', 'run_once: neuer Boot -> ok')

    # --- run_once: fehlende Datei zaehlt als Fehlversuch -----------------
    state = {}
    cfg = {'interval': 60, 'grace': 30, 'miss_needed': 3, 'cooldown': 600}
    state, d = wm.run_once(state, '/nonexistent/hb', 5, cfg)
    check(d['action'] == 'miss', 'fehlende Datei -> miss (kein Crash)')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
