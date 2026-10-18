#!/usr/bin/env python3
"""Host-Tests fuer novnc_bridge.py (reine Funktionen, kein Board, kein Netz).

Geprueft wird die Logik, die den externen Desktop-Zugang bestimmt: das Parsen
der Endpunkte, die Ableitung der oeffentlichen Proxy-URL, der websockify-argv
und die Start-/Idempotenz-Entscheidung. Kein Mock: es werden ausschliesslich
echte Funktionen des Moduls aufgerufen (Dateisystem- und Socket-Zugriffe, die
das Modul intern braucht, sind als Parameter injizierbar bzw. werden hier nicht
ausgeloest).

    python3 remote-shell/ha/test_novnc_bridge.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import novnc_bridge as b  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


def check_raises(fn, msg):
    try:
        fn()
    except ValueError:
        check(True, msg)
    else:
        check(False, msg + ' (keine ValueError)')


def test_parse_endpoint():
    check(b.parse_endpoint('0.0.0.0:12000', 'listen', '0.0.0.0')
          == ('0.0.0.0', 12000), 'parse: host:port')
    check(b.parse_endpoint('127.0.0.1:5901') == ('127.0.0.1', 5901),
          'parse: default host')
    check(b.parse_endpoint(':12000', 'listen', '0.0.0.0') == ('0.0.0.0', 12000),
          'parse: nur :port -> default host')
    check(b.parse_endpoint('6080') == ('127.0.0.1', 6080),
          'parse: nur port -> default host')
    check(b.parse_endpoint('[::1]:5901') == ('::1', 5901), 'parse: IPv6')
    check(b.parse_endpoint('[fe80::1]:12000', 'l', '0.0.0.0') == ('fe80::1', 12000),
          'parse: IPv6 ohne default')
    check_raises(lambda: b.parse_endpoint(''), 'parse: leer -> Fehler')
    check_raises(lambda: b.parse_endpoint('0.0.0.0:0'), 'parse: Port 0 -> Fehler')
    check_raises(lambda: b.parse_endpoint('0.0.0.0:99999'), 'parse: Port zu gross')
    check_raises(lambda: b.parse_endpoint('abc'), 'parse: keine Zahl')


def test_validate_port():
    check(b.validate_port('6080') == 6080, 'port: string')
    check(b.validate_port(12000) == 12000, 'port: int')
    check_raises(lambda: b.validate_port('0'), 'port: 0')
    check_raises(lambda: b.validate_port('65536'), 'port: 65536')
    check_raises(lambda: b.validate_port('x'), 'port: Text')


def test_format_endpoint():
    check(b.format_endpoint('0.0.0.0', 12000) == '0.0.0.0:12000', 'fmt: IPv4')
    check(b.format_endpoint('::1', 5901) == '[::1]:5901', 'fmt: IPv6 geklammert')
    # Rundreise
    for spec in ('0.0.0.0:12000', '127.0.0.1:5901', '[::1]:6080'):
        host, port = b.parse_endpoint(spec)
        check(b.format_endpoint(host, port) == spec, 'fmt: Rundreise %s' % spec)


def test_public_url():
    check(b.public_base_url('ABC123') ==
          'https://work-1-ABC123.prod-runtime.all-hands.dev', 'url: work-1 default')
    check(b.public_base_url('ABC123', worker=2) ==
          'https://work-2-ABC123.prod-runtime.all-hands.dev', 'url: work-2')
    check(b.public_base_url('X', domain='example.test') ==
          'https://work-1-X.example.test', 'url: eigene Domain')
    check(b.vnc_page_url('https://host/', 'vnc.html') == 'https://host/vnc.html',
          'url: Seite anhaengen, Slash normalisiert')
    check(b.vnc_page_url('https://host', '/vnc_lite.html') ==
          'https://host/vnc_lite.html', 'url: fuehrenden Slash strippen')
    check(b.vnc_page_url('https://host') == 'https://host/vnc.html',
          'url: Default-Seite')
    check_raises(lambda: b.public_base_url(''), 'url: leere runtime-id')
    check_raises(lambda: b.public_base_url('X', worker=3), 'url: worker 3')
    check_raises(lambda: b.public_base_url('X', worker='y'), 'url: worker Text')


def test_webroot_autodetect():
    seen = []

    def fake_isdir(path):
        seen.append(path)
        return path == '/opt/novnc'

    check(b.pick_webroot() is None, 'webroot: nichts da -> None')
    check(b.pick_webroot(isdir=fake_isdir) == '/opt/novnc', 'webroot: erstes Treffer')
    # Reihenfolge: /usr/share/novnc wird vor /opt/novnc geprueft.
    check(seen[0] == b.WEBROOT_CANDIDATES[0], 'webroot: Reihenfolge')
    check(b.pick_webroot(('/a', '/b'), isdir=lambda p: p == '/b') == '/b',
          'webroot: eigene Kandidaten')


def test_websockify_argv():
    argv = b.build_websockify_argv('/usr/bin/websockify', '/usr/share/novnc',
                                   ('0.0.0.0', 12000), ('127.0.0.1', 5901))
    check(argv == ['/usr/bin/websockify', '--web=/usr/share/novnc',
                   '0.0.0.0:12000', '127.0.0.1:5901'], 'argv: vollstaendig')
    argv2 = b.build_websockify_argv('ws', None, ('127.0.0.1', 12050),
                                    ('127.0.0.1', 5901))
    check(argv2 == ['ws', '127.0.0.1:12050', '127.0.0.1:5901'],
          'argv: ohne webroot kein --web')


def test_build_plan():
    plan = b.build_plan('0.0.0.0:12000', '127.0.0.1:5901', '/usr/share/novnc',
                        '/usr/bin/websockify', 'RID42')
    check(plan['listen'] == ('0.0.0.0', 12000), 'plan: listen')
    check(plan['upstream'] == ('127.0.0.1', 5901), 'plan: upstream')
    check(plan['public_base_url'] ==
          'https://work-1-RID42.prod-runtime.all-hands.dev', 'plan: base url')
    check(plan['vnc_page_url'] ==
          'https://work-1-RID42.prod-runtime.all-hands.dev/vnc.html',
          'plan: vnc url')
    check(plan['argv'][-2:] == ['0.0.0.0:12000', '127.0.0.1:5901'],
          'plan: argv Endpunkte')
    check_raises(lambda: b.build_plan('0.0.0.0:12000', '127.0.0.1:5901', '',
                                      '/w', 'RID'),
                 'plan: ohne webroot -> Fehler')
    check_raises(lambda: b.build_plan('0.0.0.0:12000', '127.0.0.1:5901',
                                      '/usr/share/novnc', '/w', ''),
                 'plan: ohne runtime-id -> Fehler')
    check_raises(lambda: b.build_plan('127.0.0.1:5901', '127.0.0.1:5901',
                                      '/usr/share/novnc', '/w', 'RID'),
                 'plan: listen == upstream -> Fehler')
    # require_webroot=False erlaubt einen webroot-losen Plan.
    plan2 = b.build_plan('127.0.0.1:12050', '127.0.0.1:5901', None, 'ws', 'RID',
                         require_webroot=False)
    check(plan2['argv'][0] == 'ws', 'plan: ohne webroot erlaubt')


def test_decide_action():
    check(b.decide_action(True, True) == 'already-running', 'decide: laeuft schon')
    check(b.decide_action(True, False) == 'already-running',
          'decide: laeuft -> Tunnel egal')
    check(b.decide_action(False, False) == 'no-tunnel', 'decide: kein Tunnel')
    check(b.decide_action(False, True) == 'start', 'decide: starten')


def test_loopback_host():
    check(b.loopback_host('0.0.0.0') == '127.0.0.1', 'loopback: wildcard')
    check(b.loopback_host('::') == '127.0.0.1', 'loopback: v6 wildcard')
    check(b.loopback_host('') == '127.0.0.1', 'loopback: leer')
    check(b.loopback_host('127.0.0.1') == '127.0.0.1', 'loopback: bleibt')
    check(b.loopback_host('192.168.1.5') == '192.168.1.5', 'loopback: fremde IP bleibt')


def test_cli_plan_and_url():
    """Die Shell-Schnittstelle (--plan/--print-url) muss stabil bleiben."""
    import contextlib
    import io

    def run(argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            rc = b.main(argv)
        return rc, buf.getvalue()

    rc, out = run(['--print-url', '--runtime-id', 'ZZZ'])
    check(rc == 0, 'cli: --print-url rc')
    check(out.strip() == 'https://work-1-ZZZ.prod-runtime.all-hands.dev/vnc.html',
          'cli: --print-url Wert')

    rc, out = run(['--plan', '--runtime-id', 'ZZZ', '--webroot', '/usr/share/novnc',
                   '--websockify', '/usr/bin/websockify'])
    check(rc == 0, 'cli: --plan rc')
    lines = dict(l.split('=', 1) for l in out.strip().splitlines())
    check(lines['BRIDGE_LISTEN'] == '0.0.0.0:12000', 'cli: plan listen')
    check(lines['BRIDGE_UPSTREAM'] == '127.0.0.1:5901', 'cli: plan upstream')
    check(lines['BRIDGE_WEBROOT'] == '/usr/share/novnc', 'cli: plan webroot')
    check(lines['BRIDGE_WEBSOCKIFY'] == '/usr/bin/websockify', 'cli: plan bin')
    check(lines['BRIDGE_PUBLIC_URL'].endswith('/vnc.html'), 'cli: plan url')

    # Der "kein Webroot"-Fehler wird in test_build_plan deterministisch geprueft
    # (build_plan), nicht ueber die CLI: dort haengt er vom Dateisystem ab.

    # Ohne --runtime-id faellt das Modul auf $RUNTIME_ID zurueck. Fuer den
    # Fehlerfall muss die Umgebung leer sein, sonst waere der Test vom
    # Sandbox-Env abhaengig.
    saved = os.environ.pop('RUNTIME_ID', None)
    try:
        rc, _ = run(['--print-url'])
    finally:
        if saved is not None:
            os.environ['RUNTIME_ID'] = saved
    check(rc == 2, 'cli: --print-url ohne runtime-id -> rc 2')


def main():
    for fn in (test_parse_endpoint, test_validate_port, test_format_endpoint,
               test_public_url, test_webroot_autodetect, test_websockify_argv,
               test_build_plan, test_decide_action, test_loopback_host,
               test_cli_plan_and_url):
        fn()
    print('novnc_bridge: %d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
