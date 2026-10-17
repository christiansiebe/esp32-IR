#!/usr/bin/env python3
"""vnc_lan_guard.py — noVNC nur aus dem LAN, ohne VNC-Passwort.

Der Nutzer will den XFCE-Desktop **ohne Passwort**, aber **nur von Geraeten im
eigenen LAN**. Damit faellt das VNC-Passwort als Schutz weg, und die Grenze muss
die Paketfilterung sein. Zwei Schichten:

  1. TigerVNC hoert nur auf 127.0.0.1 (`-localhost yes`, siehe xfce-vnc.service).
     Port 5901 ist damit von aussen gar nicht erreichbar - nur websockify auf
     demselben Host kommt dran.
  2. websockify (6080) bleibt auf 0.0.0.0, aber dieser Guard laesst in einer
     eigenen INPUT-Kette nur Quellen aus LAN_CIDR und Loopback durch. Alles
     andere wird verworfen.

Eigene Kette (`VNC_LAN`) statt Regeln direkt in INPUT: so bleibt die
Docker-/HA-Regelwelt unangetastet und der Guard laesst sich sauber entfernen.

Der Guard ist **idempotent und heilt sich**: er prueft alle CHECK_INTERVAL
Sekunden, ob Kette, INPUT-Sprung und Regeln noch stehen, und baut sie nur dann
neu auf, wenn etwas fehlt. Im Normalbetrieb schreibt er nichts (kein Flush ->
kein kurzzeitig offenes Fenster). Beim Stoppen bleiben die Regeln bewusst
stehen.

Konfiguration: /etc/default/vnc-lan-guard (KEY=value), siehe DEFAULTS unten.
Die reine Logik (`build_config`, `build_rules`, `Guard`) ist ohne Board
testbar: test_vnc_lan_guard.py injiziert einen Fake-Runner.
"""

import os
import re
import signal
import subprocess
import sys
import time
from collections import namedtuple

IPTABLES = os.environ.get('VNC_GUARD_IPTABLES', '/usr/sbin/iptables')
DEFAULTS_FILE = os.environ.get('VNC_GUARD_DEFAULTS', '/etc/default/vnc-lan-guard')

DEFAULTS = {
    'PORT': '6080',
    'LAN_CIDR': '192.168.178.0/24',
    'LO_SOURCES': '127.0.0.0/8',
    'CHAIN': 'VNC_LAN',
    'CHECK_INTERVAL': '30',
}

Config = namedtuple('Config', 'port lan_cidr lo_sources chain interval warnings')

_CIDR_RE = re.compile(r'^\d{1,3}(\.\d{1,3}){3}/\d{1,2}$')


def _valid_cidr(value):
    if not _CIDR_RE.match(value):
        raise ValueError('kein CIDR')
    octets, bits = value.split('/')
    for o in octets.split('.'):
        if int(o) > 255:
            raise ValueError('Oktett > 255')
    if int(bits) > 32:
        raise ValueError('Praefix > 32')
    return value


def parse_env_file(text):
    """KEY=value-Datei lesen; Kommentare/Leerzeilen ignorieren, Quotes strippen."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, value = line.split('=', 1)
        out[key.strip()] = value.strip().strip('"').strip("'")
    return out


def build_config(env):
    """Env-Werte pruefen; ungueltige Werte fallen mit Warnung auf den Default."""
    warnings = []

    def get(key, validate):
        default = DEFAULTS[key]
        raw = env.get(key, default)
        try:
            return validate(raw)
        except (ValueError, TypeError) as exc:
            warnings.append('%s=%r ungueltig (%s) -> Default %r'
                            % (key, raw, exc, default))
            return validate(default)

    def port(v):
        n = int(v)
        if not 1 <= n <= 65535:
            raise ValueError('ausserhalb 1..65535')
        return n

    def interval(v):
        f = float(v)
        if f <= 0:
            raise ValueError('muss > 0 sein')
        return f

    def sources(v):
        parts = [p.strip() for p in v.split(',') if p.strip()]
        return [_valid_cidr(p) for p in parts]

    def chain(v):
        if not re.match(r'^[A-Za-z0-9_]{1,28}$', v):
            raise ValueError('kein gueltiger Kettenname')
        return v

    return Config(
        port=get('PORT', port),
        lan_cidr=get('LAN_CIDR', _valid_cidr),
        lo_sources=get('LO_SOURCES', sources),
        chain=get('CHAIN', chain),
        interval=get('CHECK_INTERVAL', interval),
        warnings=warnings,
    )


def build_rules(cfg):
    """Regeln in Reihenfolge: Loopback, Loopback-Quellen, LAN, dann DROP.

    Der DROP steht bewusst zuletzt: alles, was oben nicht passt, faellt durch
    und wird verworfen. Andere Ports werden von der Kette gar nicht beruehrt
    (kein Match -> Rueckkehr nach INPUT).
    """
    port = str(cfg.port)
    rules = [['-i', 'lo', '-p', 'tcp', '--dport', port, '-j', 'ACCEPT']]
    for src in cfg.lo_sources:
        rules.append(['-s', src, '-p', 'tcp', '--dport', port, '-j', 'ACCEPT'])
    rules.append(['-s', cfg.lan_cidr, '-p', 'tcp', '--dport', port, '-j', 'ACCEPT'])
    rules.append(['-p', 'tcp', '--dport', port, '-j', 'DROP'])
    return rules


def _log(msg):
    sys.stdout.write('%s vnc-lan-guard: %s\n' % (time.strftime('%H:%M:%S'), msg))
    sys.stdout.flush()


def _run_iptables(args):
    proc = subprocess.run([IPTABLES] + args, capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


class Guard:
    """Bringt die Filterkette in den Soll-Zustand. `runner(args) -> (rc, out)`."""

    def __init__(self, cfg, runner=_run_iptables):
        self.cfg = cfg
        self.run = runner
        self.chain = cfg.chain

    def _rc(self, args):
        return self.run(args)[0]

    def ensure(self):
        """Idempotent. Rueckgabe: Liste der tatsaechlich ausgefuehrten Aenderungen."""
        actions = []

        if self._rc(['-S', self.chain]) != 0:
            self.run(['-N', self.chain])
            actions.append('Kette %s angelegt' % self.chain)

        if self._rc(['-C', 'INPUT', '-j', self.chain]) != 0:
            self.run(['-I', 'INPUT', '1', '-j', self.chain])
            actions.append('INPUT-Sprung eingefuegt')

        rules = build_rules(self.cfg)
        if not all(self._rc(['-C', self.chain] + r) == 0 for r in rules):
            self.run(['-F', self.chain])
            for rule in rules:
                self.run(['-A', self.chain] + rule)
            actions.append('%d Regeln neu gesetzt' % len(rules))

        return actions

    def remove(self):
        """Kette und Sprung entfernen (Notausgang). Regeln in INPUT bleiben sonst."""
        if self._rc(['-C', 'INPUT', '-j', self.chain]) == 0:
            self.run(['-D', 'INPUT', '-j', self.chain])
        if self._rc(['-S', self.chain]) == 0:
            self.run(['-F', self.chain])
            self.run(['-X', self.chain])


def load_config():
    text = ''
    try:
        with open(DEFAULTS_FILE) as fh:
            text = fh.read()
    except OSError:
        pass
    return build_config(parse_env_file(text))


def main(argv):
    cfg = load_config()

    if '--print' in argv:
        print('LAN_CIDR=%s PORT=%d CHAIN=%s INTERVAL=%ss'
              % (cfg.lan_cidr, cfg.port, cfg.chain, cfg.interval))
        for rule in build_rules(cfg):
            print('  -A %s %s' % (cfg.chain, ' '.join(rule)))
        return 0

    for warning in cfg.warnings:
        _log('WARNUNG: ' + warning)

    guard = Guard(cfg)
    if '--remove' in argv:
        guard.remove()
        _log('Kette entfernt')
        return 0

    if '--check' in argv:
        actions = guard.ensure()
        _log('; '.join(actions) if actions else 'alles in Ordnung')
        return 0

    stopping = {'flag': False}

    def _stop(_signum, _frame):
        stopping['flag'] = True

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    _log('aktiv: Port %d, nur %s + Loopback, alle %gs Pruefung'
         % (cfg.port, cfg.lan_cidr, cfg.interval))
    while not stopping['flag']:
        actions = guard.ensure()
        if actions:
            _log('; '.join(actions))
        # In kleinen Schritten schlafen, damit SIGTERM schnell greift.
        waited = 0.0
        while waited < cfg.interval and not stopping['flag']:
            time.sleep(min(1.0, cfg.interval - waited))
            waited += 1.0
    # Regeln bleiben absichtlich stehen: der Schutz soll einen Dienst-Stopp
    # ueberleben (nur ein bewusstes --remove nimmt ihn weg).
    _log('beendet, Regeln bleiben aktiv')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
