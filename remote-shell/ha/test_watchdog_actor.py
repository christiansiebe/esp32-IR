#!/usr/bin/env python3
"""Host tests for watchdog_actor.py -- pure logic, no board, no network.

Checked: request/plan builders, the cooldown/quorum gate, the pulse command
builder, the state file and the dry-run default. The real actuator (power cut)
needs hardware and is deliberately NOT part of this test; `execute_plan` is only
reached with `--execute`, which the tests never pass.
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import watchdog_actor as wa  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def raises(fn, *a, **kw):
    try:
        fn(*a, **kw)
    except ValueError:
        return True
    except Exception:  # noqa: BLE001 - any other error is a test failure
        return False
    return False


def main():
    # --- build_http_request ---------------------------------------------
    r = wa.build_http_request('http://plug.local/cm?cmnd=Power%20TOGGLE')
    check(r['method'] == 'GET' and r['url'].endswith('TOGGLE'), 'http GET url')
    check(r['body'] is None and 'Content-Length' not in r['headers'],
          'GET ohne Body -> kein Content-Length')
    check(r['timeout'] == wa.DEFAULTS['request_timeout'], 'http default timeout')

    r = wa.build_http_request('http://plug/relay', method='post', body='ON',
                              timeout=3)
    check(r['method'] == 'POST', 'method wird upper-cased')
    check(r['body'] == b'ON', 'body wird utf-8-kodiert')
    check(r['headers']['Content-Length'] == '2', 'Content-Length = Laenge')
    check(r['headers']['Content-Type'].startswith('text/plain'), 'Content-Type')
    check(r['timeout'] == 3, 'timeout wird uebernommen')

    r = wa.build_http_request('http://plug/relay', method='POST')
    check(r['body'] == b'' and r['headers']['Content-Length'] == '0',
          'POST ohne Body -> leerer Body, Laenge 0')

    check(raises(wa.build_http_request, ''), 'http ohne url -> ValueError')
    check(raises(wa.build_http_request, 'http://x', method='DELETE'),
          'unbekannte Methode -> ValueError')

    # --- build_mqtt_request ---------------------------------------------
    r = wa.build_mqtt_request('cmnd/plug/Power', 'TOGGLE')
    check(r['argv'] == ['mosquitto_pub', '-t', 'cmnd/plug/Power', '-m', 'TOGGLE'],
          'mqtt argv minimal')
    check(r['retain'] is False and r['username'] is None, 'mqtt Defaults')

    r = wa.build_mqtt_request('tasmota/plug/state', 'ON', retain=True,
                              username='ha')
    check(r['argv'] == ['mosquitto_pub', '-t', 'tasmota/plug/state', '-m', 'ON',
                        '-r', '-u', 'ha'], 'mqtt argv mit retain + user')
    check(raises(wa.build_mqtt_request, ''), 'mqtt ohne topic -> ValueError')

    # --- build_pulse_command --------------------------------------------
    check(wa.build_pulse_command('usbrelay R=0', 'usbrelay R=1', 5) ==
          'usbrelay R=0; sleep 5; usbrelay R=1', 'puls 5 s ohne .0')
    check(wa.build_pulse_command('off', 'on', 0.5) ==
          'off; sleep 0.5; on', 'puls mit Dezimalwert')
    check(raises(wa.build_pulse_command, '', 'on', 5), 'puls ohne off -> Fehler')
    check(raises(wa.build_pulse_command, 'off', '', 5), 'puls ohne on -> Fehler')
    check(raises(wa.build_pulse_command, 'off', 'on', -1),
          'negativer Puls -> Fehler')

    # --- build_plan ------------------------------------------------------
    p = wa.build_plan('http', url='http://plug/relay', method='GET')
    check(p['kind'] == 'http' and p['target'] == 'http://plug/relay',
          'plan http target')
    p = wa.build_plan('mqtt', topic='cmnd/plug/Power', payload='TOGGLE')
    check(p['kind'] == 'mqtt' and p['target'] == 'cmnd/plug/Power'
          and p['payload'] == 'TOGGLE', 'plan mqtt')
    p = wa.build_plan('command', command='ssh powercut cycle')
    check(p['kind'] == 'command' and p['argv'] == ['/bin/sh', '-c',
                                                   'ssh powercut cycle'],
          'plan command argv')
    check(raises(wa.build_plan, 'command'), 'command ohne Befehl -> Fehler')
    check(raises(wa.build_plan, 'carrier-pigeon'), 'unbekannter kind -> Fehler')

    # --- format_plan -----------------------------------------------------
    check('HTTP GET' in wa.format_plan(wa.build_plan('http', url='http://x')),
          'format_plan http')
    check('MQTT publish' in wa.format_plan(wa.build_plan('mqtt', topic='t')),
          'format_plan mqtt')
    check(wa.format_plan(wa.build_plan('command', command='c')) == 'CMD c',
          'format_plan command')

    # --- cooldown_ok / quorum_ok ----------------------------------------
    check(wa.cooldown_ok(100, None, 600) is True, 'kein last_fired -> ok')
    check(wa.cooldown_ok(699, 100, 600) is False, '599 s < 600 -> blockiert')
    check(wa.cooldown_ok(700, 100, 600) is True, 'genau 600 s -> ok')
    check(wa.cooldown_ok(701, 100, 600) is True, '601 s -> ok')
    check(wa.quorum_ok([True, False, True, True]) == 3, 'quorum zaehlt True')
    check(wa.quorum_ok([]) == 0, 'quorum leer -> 0')

    # --- evaluate --------------------------------------------------------
    allowed, reason = wa.evaluate(100, None, 600)
    check(allowed is True and 'erlaubt' in reason, 'evaluate: erster Trigger ok')

    allowed, reason = wa.evaluate(100, 50, 600)
    check(allowed is False and 'Cooldown' in reason, 'evaluate: Cooldown blockt')

    allowed, reason = wa.evaluate(700, 100, 600)
    check(allowed is True, 'evaluate: Cooldown abgelaufen -> ok')

    allowed, reason = wa.evaluate(100, None, 600, votes=[True, False],
                                  min_quorum=2)
    check(allowed is False and 'Quorum' in reason,
          'evaluate: Quorum 1/2 -> blockiert')

    allowed, reason = wa.evaluate(100, None, 600, votes=[True, True],
                                  min_quorum=2)
    check(allowed is True, 'evaluate: Quorum 2/2 -> ok')

    # Quorum wird VOR dem Cooldown geprueft: fehlendes Quorum meldet Quorum.
    allowed, reason = wa.evaluate(100, 50, 600, votes=[False], min_quorum=1)
    check(allowed is False and 'Quorum' in reason,
          'evaluate: Quorum hat Vorrang vor Cooldown')

    # --- state file ------------------------------------------------------
    with tempfile.TemporaryDirectory() as td:
        sf = os.path.join(td, 'state')
        check(wa._read_last_fired(sf) is None, 'state: fehlende Datei -> None')
        wa._write_last_fired(sf, 1234.0)
        check(wa._read_last_fired(sf) == 1234.0, 'state: round-trip')
        with open(sf, 'w') as fh:
            fh.write('keine-zahl\n')
        check(wa._read_last_fired(sf) is None, 'state: Muell -> None')
        check(wa._read_last_fired('') is None, 'state: leerer Pfad -> None')

    # --- main: Trockenlauf ist der Default (nichts wird gesendet) --------
    rc = wa.main(['--kind', 'command', '--command', 'echo should-not-run',
                  '--now', '1000'])
    check(rc == 0, 'main: Dry-Run exit 0')

    # Cooldown blockt -> exit 0, kein Fehler.
    rc = wa.main(['--kind', 'command', '--command', 'echo x',
                  '--now', '1000', '--last-fired', '900', '--cooldown', '600'])
    check(rc == 0, 'main: Cooldown -> exit 0')

    # Ungueltiger Plan -> exit 2.
    rc = wa.main(['--kind', 'http', '--now', '1000'])
    check(rc == 2, 'main: fehlende url -> exit 2')
    rc = wa.main(['--kind', 'command', '--now', '1000'])
    check(rc == 2, 'main: fehlender Befehl -> exit 2')
    rc = wa.main(['--kind', 'command', '--command', 'x', '--off-cmd', 'off',
                  '--now', '1000'])
    check(rc == 2, 'main: halber Puls -> exit 2')

    # --- main: Puls-Kommando wird zusammengebaut, aber nicht ausgefuehrt ---
    rc = wa.main(['--kind', 'command', '--off-cmd', 'off', '--on-cmd', 'on',
                  '--pulse-seconds', '7', '--now', '1000'])
    check(rc == 0, 'main: Puls-Dry-Run exit 0')

    print('%d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
