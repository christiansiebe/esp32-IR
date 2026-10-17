#!/usr/bin/env python3
"""Host-Tests fuer vnc_lan_guard.py (kein Board, kein Netz).

Geprueft wird die reine Logik: Config-Parsing/Validierung, das Regelwerk und
die idempotente `Guard.ensure()` gegen einen Fake-Runner, der einen echten
iptables-Zustand nachbildet. Der echte iptables-Aufruf ist durch den Lauf auf
dem Odroid abgedeckt.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vnc_lan_guard as g  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print('  FAIL:', msg)


class FakeIptables:
    """Bildet gerade so viel iptables nach, wie der Guard benutzt."""

    def __init__(self):
        self.chains = set()
        self.rules = {}          # chain -> list[tuple]
        self.jumps = []          # INPUT-Spruenge
        self.calls = []

    def __call__(self, args):
        self.calls.append(args)
        verb = args[0]

        if verb == '-S':
            target = args[1]
            if target == 'INPUT':
                return 0, '\n'.join('-j ' + c for c in self.jumps)
            if target in self.chains:
                body = ''.join(' -A %s %s\n' % (target, ' '.join(r))
                               for r in self.rules.get(target, []))
                return 0, body
            return 1, 'iptables: No chain/target/match by that name.'

        if verb == '-C':
            target = args[1]
            rest = tuple(args[2:])
            if target == 'INPUT' and rest == ('-j', self._self_chain()):
                return (0, '') if self._self_chain() in self.jumps else (1, '')
            if target in self.chains:
                return (0, '') if rest in self.rules.get(target, []) else (1, '')
            return 1, 'no chain'

        if verb == '-N':
            self.chains.add(args[1])
            self.rules[args[1]] = []
            return 0, ''
        if verb == '-F':
            self.rules[args[1]] = []
            return 0, ''
        if verb == '-X':
            self.chains.discard(args[1])
            self.rules.pop(args[1], None)
            return 0, ''
        if verb == '-A':
            self.rules[args[1]].append(tuple(args[2:]))
            return 0, ''
        if verb == '-I':
            # -I INPUT 1 -j CHAIN
            if args[1] == 'INPUT' and '-j' in args:
                self.jumps.insert(0, args[args.index('-j') + 1])
            return 0, ''
        if verb == '-D':
            # -D INPUT -j CHAIN
            if args[1] == 'INPUT' and '-j' in args:
                self.jumps.remove(args[args.index('-j') + 1])
            return 0, ''
        return 0, ''

    def _self_chain(self):
        return self._chain

    def bind(self, chain):
        self._chain = chain


def new_guard(**env):
    base = {'LAN_CIDR': '192.168.178.0/24', 'PORT': '6080'}
    base.update(env)
    cfg = g.build_config(base)
    fake = FakeIptables()
    fake.bind(cfg.chain)
    return cfg, fake, g.Guard(cfg, runner=fake)


def main():
    # --- Config-Parsing ---
    parsed = g.parse_env_file('# Kommentar\nPORT=6080\nLAN_CIDR="192.168.178.0/24"\n\nCHAIN = VNC_LAN\n')
    check(parsed['PORT'] == '6080', 'parse: PORT')
    check(parsed['LAN_CIDR'] == '192.168.178.0/24', 'parse: Quotes gestrippt')
    check(parsed['CHAIN'] == 'VNC_LAN', 'parse: Leerzeichen um Key')

    cfg = g.build_config({'LAN_CIDR': '10.0.0.0/8', 'PORT': '6080', 'CHECK_INTERVAL': '15'})
    check(cfg.port == 6080 and cfg.interval == 15.0, 'config: Port/Intervall')
    check(cfg.warnings == [], 'config: gueltig -> keine Warnung')

    # --- Validierung faellt auf Default zurueck ---
    bad = g.build_config({'LAN_CIDR': '192.168.178.0/99', 'PORT': '70000',
                          'LO_SOURCES': '127.0.0.0/8,quatsch', 'CHECK_INTERVAL': '0'})
    check(bad.lan_cidr == '192.168.178.0/24', 'config: Praefix>32 -> Default')
    check(bad.port == 6080, 'config: Port 70000 -> Default')
    check(bad.interval == 30.0, 'config: Intervall 0 -> Default')
    check(len(bad.warnings) == 4, 'config: 4 Warnungen, war %d' % len(bad.warnings))

    ok = g.build_config({'LO_SOURCES': '127.0.0.0/8,127.0.1.1/32'})
    check(ok.lo_sources == ['127.0.0.0/8', '127.0.1.1/32'], 'config: Loopback-Liste')

    # --- Regelwerk ---
    cfg = g.build_config({'LAN_CIDR': '192.168.178.0/24', 'PORT': '6080'})
    rules = g.build_rules(cfg)
    flat = [' '.join(r) for r in rules]
    check(flat[0] == '-i lo -p tcp --dport 6080 -j ACCEPT', 'rules: Loopback zuerst')
    check(any('-s 127.0.0.0/8' in f for f in flat), 'rules: Loopback-Quelle')
    check(any('-s 192.168.178.0/24' in f for f in flat), 'rules: LAN-Quelle')
    check(flat[-1] == '-p tcp --dport 6080 -j DROP', 'rules: DROP zuletzt')
    check(len(rules) == 4, 'rules: 4 Regeln, war %d' % len(rules))

    # --- ensure() aus dem Nichts ---
    cfg, fake, guard = new_guard()
    actions = guard.ensure()
    check(len(actions) == 3, 'ensure: Kette+Sprung+Regeln, war %r' % actions)
    check(cfg.chain in fake.chains, 'ensure: Kette existiert')
    check(cfg.chain in fake.jumps, 'ensure: INPUT-Sprung existiert')
    check(fake.rules[cfg.chain] == [tuple(r) for r in g.build_rules(cfg)],
          'ensure: Regeln korrekt')

    # --- Idempotenz: zweiter Lauf aendert nichts ---
    before = len(fake.calls)
    actions = guard.ensure()
    check(actions == [], 'ensure: zweiter Lauf ist ein No-op')
    # Nur Leseaufrufe (-S/-C), keine schreibenden -N/-I/-F/-A.
    writes = [c for c in fake.calls[before:] if c[0] in ('-N', '-I', '-F', '-A', '-D', '-X')]
    check(writes == [], 'ensure: keine Schreibzugriffe im stabilen Zustand')

    # --- Halb zerstoerter Zustand: fehlende Regeln werden wiederhergestellt ---
    fake.rules[cfg.chain] = fake.rules[cfg.chain][:1]
    actions = guard.ensure()
    check(actions == ['4 Regeln neu gesetzt'], 'ensure: heilt fehlende Regeln')
    check(fake.rules[cfg.chain] == [tuple(r) for r in g.build_rules(cfg)],
          'ensure: nach Heilung wieder vollstaendig')

    # --- Sprung verloren -> wieder eingefuegt ---
    fake.jumps.clear()
    actions = guard.ensure()
    check('INPUT-Sprung eingefuegt' in actions, 'ensure: Sprung wiederhergestellt')

    # --- DROP-Regel ist die letzte, sonst waere der Schutz wirkungslos ---
    last = fake.rules[cfg.chain][-1]
    check(last[-2:] == ('-j', 'DROP'), 'ensure: letzte Regel ist DROP')

    # --- remove() raeumt auf ---
    guard.remove()
    check(cfg.chain not in fake.chains, 'remove: Kette weg')
    check(cfg.chain not in fake.jumps, 'remove: Sprung weg')

    # --- Anderer Port wird von der Kette nicht beruehrt ---
    cfg2 = g.build_config({'PORT': '8080'})
    for r in g.build_rules(cfg2):
        check('8080' in ' '.join(r), 'rules: nur der konfigurierte Port')

    print('vnc_lan_guard: %d Checks, %d Fehler' % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
