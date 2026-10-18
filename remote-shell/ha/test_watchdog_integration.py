#!/usr/bin/env python3
"""Integration test for the off-board watchdog: monitor -> wrapper -> actor.

No hardware, no network, no mocks. It ties the three real pieces together:

  * watchdog_monitor.run_once / decide  -- the observer's decision
  * watchdog_powercut.sh                -- the actuator entry point (dry run)
  * watchdog_actor.evaluate/state file  -- the durable cooldown gate

Scenario: a simulated heartbeat stops. Over the timeline the monitor must plan
exactly ONE power cut; the wrapper must render it as a valid dry-run plan
(nothing switched). A second outage inside the durable cooldown must be
suppressed, and a third outage after the cooldown must plan a cut again.

The real power cut needs hardware and is deliberately NOT exercised: the wrapper
runs unarmed, so watchdog_actor.py never receives --execute.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import watchdog_monitor as wm   # noqa: E402
import watchdog_actor as wa     # noqa: E402

WRAPPER = os.path.join(HERE, 'watchdog_powercut.sh')
ACTOR = os.path.join(HERE, 'watchdog_actor.py')

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def run_wrapper(now, state_file):
    """Run the wrapper in dry-run (unarmed) and return its stdout+stderr.

    WATCHDOG_COMMAND='true' is only the stand-in used to BUILD a plan; in a dry
    run it is never executed. WATCHDOG_POWERCUT_ARMED is deliberately not set.
    """
    env = dict(os.environ)
    env.update({
        'WATCHDOG_ACTOR': ACTOR,
        'WATCHDOG_ACTOR_KIND': 'command',
        'WATCHDOG_COMMAND': 'true',
        'WATCHDOG_STATE_FILE': state_file,
        'WATCHDOG_COOLDOWN': '900',
        'WATCHDOG_NOW': str(now),
    })
    env.pop('WATCHDOG_POWERCUT_ARMED', None)
    proc = subprocess.run(['sh', WRAPPER], env=env, capture_output=True, text=True)
    return proc.returncode, (proc.stdout or '') + (proc.stderr or '')


def main():
    cfg = {'interval': 60, 'grace': 30, 'miss_needed': 3, 'cooldown': 900}

    with tempfile.TemporaryDirectory() as td:
        hb = os.path.join(td, 'board-heartbeat')
        state = os.path.join(td, 'actor-state')

        # --- the simulated outage through the monitor's pure logic ----------
        with open(hb, 'w') as fh:
            fh.write('1000 bootA 1\n')
        mon_state = {}
        reboots = []
        actions = []
        # t=1000 fresh, then the heartbeat stops: 1100/1110 miss, 1120 reboot,
        # 1130 cooldown, 1140 cooldown, 2100 reboot (after the 900 s cooldown).
        for now in (1000, 1100, 1110, 1120, 1130, 1140, 2100):
            mon_state, d = wm.run_once(mon_state, hb, now, cfg)
            actions.append(d['action'])
            if d['action'] == 'reboot':
                reboots.append(now)

        check(actions[0] == 'ok', 'Monitor: frischer Heartbeat -> ok')
        check(actions[1:3] == ['miss', 'miss'], 'Monitor: erst Fehlversuche')
        check(reboots == [1120, 2100],
              'Monitor: genau die erwarteten zwei Reboot-Zeitpunkte (Cooldown '
              'blockt 1130/1140), bekam %r' % reboots)
        # Genau EIN Power-Cut pro Ausfall-Fenster: im Fenster 1100..1140 nur
        # ein 'reboot'.
        window = [a for a in actions[1:6]]
        check(window.count('reboot') == 1,
              'Monitor: genau EIN Reboot im ersten Ausfall-Fenster, bekam %r'
              % window)

        # --- the wrapper turns that decision into a dry-run plan ------------
        rc, out = run_wrapper(1120, state)
        check(rc == 0, 'Wrapper: Dry-Run exit 0 (bekam %d)' % rc)
        check('Gate: erlaubt' in out, 'Wrapper: Gate erlaubt (erster Ausfall)')
        check('CMD true' in out, 'Wrapper: baut den Aktor-Plan')
        check('Trockenlauf' in out, 'Wrapper: sendet nichts (Dry-Run Default)')
        check('FEUERE' not in out, 'Wrapper: Aktor wird NICHT ausgeloest')

        # --- durable cooldown: a real cut would have recorded t=1120 --------
        # The actor writes the state file only after a successful execute; here
        # we seed it to represent that recorded cut (the durable half).
        wa._write_last_fired(state, 1120.0)

        # Second outage inside the cooldown (1120 + 900 = 2020): suppressed.
        rc, out = run_wrapper(1500, state)
        check(rc == 0, 'Wrapper: blockierter Lauf ist exit 0 (bekam %d)' % rc)
        check('Gate: blockiert' in out and 'Cooldown' in out,
              'Wrapper: zweiter Ausfall im Cooldown wird unterdrueckt')
        check('Trockenlauf' not in out and 'FEUERE' not in out,
              'Wrapper: blockierter Lauf plant/sendet keinen Cut')

        # Third outage after the cooldown: plans a cut again.
        rc, out = run_wrapper(2100, state)
        check(rc == 0, 'Wrapper: Lauf nach Cooldown exit 0')
        check('Gate: erlaubt' in out and 'Trockenlauf' in out,
              'Wrapper: nach Cooldown wieder ein Plan')

        # --- the durable state survives a fresh process (no in-RAM state) ---
        check(wa._read_last_fired(state) == 1120.0,
              'durable Cooldown-Zustand liegt in der Datei, nicht im RAM')

        # --- arming is explicit: --armed would pass --execute ----------------
        # We do NOT arm here; instead assert the gate is what blocks, not a bug.
        allowed, reason = wa.evaluate(1500, wa._read_last_fired(state), 900)
        check(allowed is False and 'Cooldown' in reason,
              'reine evaluate(): Cooldown blockt den zweiten Ausfall')
        allowed, reason = wa.evaluate(2100, wa._read_last_fired(state), 900)
        check(allowed is True, 'reine evaluate(): nach Cooldown erlaubt')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
