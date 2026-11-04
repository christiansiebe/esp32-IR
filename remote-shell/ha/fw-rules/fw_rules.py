#!/usr/bin/env python3
"""fw_rules.py - Default-deny INPUT-Firewall fuer den Odroid N2.

Warum: INPUT stand auf ACCEPT; gefiltert war nur noVNC (6080, vnc_lan_guard).
Alles andere, was lauscht, war fuer JEDEN im LAN erreichbar - ohne Passwort.
Beispiele aus dem Ist-Zustand: cupsd (631), meter_server (8099, liefert das
Bild fuer das FRITZ!Fon), go2rtc (18555).

Bauweise: eine EIGENE Kette (`FW`), die in INPUT an Position 1 springt und mit
einem abschliessenden `-j DROP` endet; zusaetzlich steht die INPUT-Policy auf
DROP. Damit
  * laesst sich die Firewall mit EINEM Kommando entfernen (--rollback),
  * bleibt die bestehende Regelwelt (VNC_LAN, Docker) unangetastet,
  * gilt fail-closed: was die Kette nicht erlaubt, wird verworfen.

Das abschliessende DROP ist Absicht und nicht nur Kosmetik: ohne es haengt die
Wirkung allein an der INPUT-Policy, und jede Regel, die in INPUT HINTER dieser
Kette steht, waere ein Loch. Genau so eine lag hier herum (`--dport 8125 -j
ACCEPT`, von jeder Quelle, ohne dass 8125 ueberhaupt lauschte). Mit dem
terminalen DROP ist die Kette in sich abgeschlossen.

Die erlaubten Quellnetze kommen aus den ECHTEN LAN-Adressen
(`ip -j addr show`), wie bei chat_allow.py - die FRITZ!Box delegiert das
IPv6-/64 bei jedem Router Advertisement neu, ein fester Wert waere nach dem
naechsten Reboot falsch.

Wichtig - was diese Firewall NICHT schuetzt: Docker-veroeffentlichte Ports
(MQTT 1883/1884/8883/8884, observer 4357) laufen NICHT ueber INPUT. Docker
schickt sie per DNAT durch FORWARD. Gemessen mit einem netns: Paket an 1883
-> INPUT 0 Pakete, FORWARD 4 Pakete. Dafuer waere die Kette DOCKER-USER
zustaendig, nicht INPUT.

Konfiguration: /etc/default/fw-rules (KEY=value), siehe DEFAULT_CFG.
Tests: test_fw_rules.py (laeuft ohne Board, kein iptables noetig).
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chat_allow  # LAN-Erkennung wird wiederverwendet, nicht kopiert

IPTABLES = os.environ.get("FW_IPTABLES", "/usr/sbin/iptables-nft")
IP6TABLES = os.environ.get("FW_IP6TABLES", "/usr/sbin/ip6tables-nft")
DEFAULTS = os.environ.get("FW_DEFAULTS", "/etc/default/fw-rules")
LOCK = "/run/fw-rules.lock"

CHAIN = "FW"

DEFAULT_CFG = {
    # Dienste, die aus dem LAN erreichbar bleiben MUESSEN. Jeder Eintrag ist
    # bewusst: 22 ssh, 80/443 nginx (HA, Chat), 6080 noVNC, 631 Drucken,
    # 1883/1884/8883/8884 MQTT (Mosquitto, laeuft ueber docker-proxy -> INPUT;
    # ohne diese waeren die ESP-Geraete abgeschnitten), 8081 Perses,
    # 8099 meter_server (FRITZ!Fon), 8123 HA direkt, 18555 go2rtc,
    # 39023 HA-eigener Port.
    "LAN_TCP_PORTS": "22,80,443,6080,631,1883,1884,8883,8884,8081,8099,8123,8765,18555,39023",
    "LAN_UDP_PORTS": "",
    "ALLOW_ICMP": "1",
    "ALLOW_GLOBAL_IPV6": "1",
    # Docker-Bruecken duerfen den Host bedienen (Addons -> Host-Dienste).
    "DOCKER_V4": "172.17.0.0/16,172.30.32.0/23",
    "DOCKER_V6": "fd0c:ac1e:2100::/48",
}

# Immer erlaubt, unabhaengig von der Quelle: DHCP-Antwort und die
# Discovery-Ports (mDNS/LLMNR/SSDP), die Home Assistant fuer die Geraete-
# erkennung braucht. Sie sind nicht routbar bzw. rein lokal.
ALWAYS_UDP_V4 = (68, 1900, 5353, 5355)
ALWAYS_UDP_V6 = (546, 1900, 5353, 5355)


def truthy(v):
    return chat_allow.truthy(v)


def parse_ports(text):
    """'443, 80' -> [80, 443]. Leerstring -> []. Ungueltig -> ValueError."""
    out = []
    for part in str(text or "").replace(" ", "").split(","):
        if not part:
            continue
        n = int(part)
        if not 1 <= n <= 65535:
            raise ValueError("Port ausserhalb 1..65535: %s" % part)
        out.append(n)
    return sorted(set(out))


def parse_nets(text):
    return [n.strip() for n in str(text or "").split(",") if n.strip()]


def load_defaults(path=DEFAULTS):
    cfg = dict(DEFAULT_CFG)
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    return cfg


def _common(cfg, family):
    """Kopf der Kette: Loopback, bestehende Verbindungen, ICMP, Discovery."""
    r = [["-i", "lo", "-j", "ACCEPT"],
         ["-m", "conntrack", "--ctstate", "ESTABLISHED,RELATED", "-j", "ACCEPT"],
         ["-m", "conntrack", "--ctstate", "INVALID", "-j", "DROP"]]
    if truthy(cfg.get("ALLOW_ICMP", "1")):
        r.append(["-p", "ipv6-icmp" if family == 6 else "icmp", "-j", "ACCEPT"])
        if family == 4:
            r.append(["-p", "igmp", "-j", "ACCEPT"])
    for p in (ALWAYS_UDP_V6 if family == 6 else ALWAYS_UDP_V4):
        r.append(["-p", "udp", "--dport", str(p), "-j", "ACCEPT"])
    for net in parse_nets(cfg.get("DOCKER_V6" if family == 6 else "DOCKER_V4")):
        r.append(["-s", net, "-j", "ACCEPT"])
    return r


def _lan_rules(cfg, nets):
    # iptables --multiport erlaubt MAXIMAL 15 Ports je Regel. Mehr -> "too many
    # ports specified" -> Regel wird still NICHT gesetzt -> Firewall droppt das
    # LAN (Lockout). Deshalb in Bloecke zu <=15 aufteilen. (Regression 2026-11-04.)
    def blocks(ports):
        ports = [str(p) for p in ports]
        return [ports[i:i + 15] for i in range(0, len(ports), 15)]
    tcp = blocks(parse_ports(cfg.get("LAN_TCP_PORTS")))
    udp = blocks(parse_ports(cfg.get("LAN_UDP_PORTS")))
    r = []
    for net in nets:
        for chunk in tcp:
            r.append(["-s", net, "-p", "tcp", "-m", "multiport",
                      "--dports", ",".join(chunk), "-j", "ACCEPT"])
        for chunk in udp:
            r.append(["-s", net, "-p", "udp", "-m", "multiport",
                      "--dports", ",".join(chunk), "-j", "ACCEPT"])
    return r


def build_v4(cfg, lan_v4):
    """Regeln der Kette FW (IPv4). Rein, ohne Seiteneffekte, testbar."""
    return _common(cfg, 4) + _lan_rules(cfg, lan_v4) + [["-j", "DROP"]]


def build_v6(cfg, lan_v6):
    """Regeln der Kette FW (IPv6). Rein, ohne Seiteneffekte, testbar."""
    return _common(cfg, 6) + _lan_rules(cfg, lan_v6) + [["-j", "DROP"]]


def _run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def canon_rule(tokens):
    """Regel in eine vergleichbare Form bringen.

    `iptables -S` gibt Regeln normalisiert aus: aus `--ctstate ESTABLISHED,RELATED`
    wird `RELATED,ESTABLISHED`, und `-p udp --dport 68` wird zu
    `-p udp -m udp --dport 68`. Ein direkter String-Vergleich schluege deshalb
    IMMER fehl - die Kette wuerde alle 5 min grundlos neu geschrieben. Hier wird
    beides eingeebnet: implizite `-m <proto>` fliegen raus, Wertelisten werden
    sortiert.
    """
    proto = None
    out = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t == "-p" and i + 1 < len(tokens):
            proto = tokens[i + 1]
            out += ["-p", proto]
            i += 2
            continue
        if t == "-m" and i + 1 < len(tokens) and tokens[i + 1] == proto:
            i += 2                      # implizit durch -p, nicht vergleichen
            continue
        if t in ("--ctstate", "--dports", "--sports") and i + 1 < len(tokens):
            out += [t, ",".join(sorted(tokens[i + 1].split(",")))]
            i += 2
            continue
        out.append(t)
        i += 1
    return tuple(out)


def chain_matches(ipt, chain, rules, runner=_run):
    """True, wenn die Kette GENAU diese Regeln in dieser Reihenfolge hat.

    Ein reiner Anwesenheitstest genuegt nicht: eine ueberzaehlige alte Regel
    (z. B. die Adresse eines Test-Interfaces) bliebe sonst fuer immer stehen.
    Die Kette waere dann stiller Ballast und, schlimmer, koennte eine Quelle
    erlauben, die laengst nicht mehr existiert.
    """
    r = runner([ipt, "-S", chain])
    if r.returncode != 0:
        return False
    want = [canon_rule(x) for x in rules]
    have = []
    for line in (r.stdout or "").splitlines():
        if not line.startswith("-A "):
            continue
        parts = line.split()
        assert parts[1] == chain
        have.append(canon_rule(parts[2:]))
    return have == want


def ensure_chain(ipt, chain, rules, runner=_run):
    """Kette + Regeln + INPUT-Sprung in den Soll-Zustand bringen (idempotent).

    Liefert (changes, failures). failures ist NICHT leer, wenn ein
    iptables-Aufruf fehlschlaegt (z. B. --multiport mit >15 Ports, oder ein
    unbekanntes Modul). Frueher wurden die Rueckgabecodes ignoriert -> die
    Regel fehlte still und INPUT ging trotzdem auf DROP (Lockout 2026-11-04).
    """
    changes, failures = [], []
    if runner([ipt, "-S", chain]).returncode != 0:
        if runner([ipt, "-N", chain]).returncode != 0:
            failures.append("Kette %s konnte nicht angelegt werden" % chain)
        changes.append("Kette %s angelegt" % chain)
    if not chain_matches(ipt, chain, rules, runner=runner):
        runner([ipt, "-F", chain])
        for rule in rules:
            r = runner([ipt, "-A", chain] + rule)
            if r.returncode != 0:
                failures.append("Regel fehlgeschlagen: %s (%s)"
                                % (" ".join(rule), (r.stderr or "").strip()[:80]))
        changes.append("%d Regeln gesetzt" % len(rules))
    if runner([ipt, "-C", "INPUT", "-j", chain]).returncode != 0:
        if runner([ipt, "-I", "INPUT", "1", "-j", chain]).returncode != 0:
            failures.append("INPUT-Sprung konnte nicht eingefuegt werden")
        changes.append("INPUT-Sprung eingefuegt")
    return changes, failures


def set_policy(ipt, policy, runner=_run):
    return runner([ipt, "-P", "INPUT", policy]).returncode == 0


def verify(ipt, chain, rules, runner=_run):
    """Selbstpruefung: sitzt JEDE beabsichtigte Regel wirklich in der Kette?

    Der gefaehrlichste Zustand waere: Policy DROP, aber eine ACCEPT-Regel fehlt
    -> der Host ist von aussen unerreichbar (Lockout). Diese Pruefung ist die
    Bremse und prueft deshalb jede Regel einzeln per `iptables -C`, nicht nur
    die erste. Fehlt eine (z. B. weil --multiport >15 Ports still abgelehnt
    wurde), wird DROP NICHT gesetzt.
    """
    problems = []
    if runner([ipt, "-C", "INPUT", "-j", chain]).returncode != 0:
        problems.append("INPUT-Sprung fehlt")
    if not rules:
        problems.append("keine Regeln")
        return problems
    for rule in rules:
        if runner([ipt, "-C", chain] + rule).returncode != 0:
            problems.append("Regel fehlt: %s" % " ".join(rule))
    return problems


def lan_cidrs(cfg):
    """Erlaubte LAN-Netze, getrennt nach Familie."""
    raw = subprocess.check_output(["ip", "-j", "addr", "show"], text=True)
    ifaces = json.loads(raw)
    cidrs = chat_allow.allowed_cidrs(
        ifaces, allow_global=truthy(cfg.get("ALLOW_GLOBAL_IPV6", "1")))
    return ([c for c in cidrs if ":" not in c],
            [c for c in cidrs if ":" in c])


def has_real_lan(cidrs):
    """True, wenn ein echtes LAN-Netz dabei ist (nicht nur Loopback/link-local).

    Wichtig als Bremse: `allowed_cidrs` liefert IMMER 127.0.0.1/32, ::1/128 und
    fe80::/10. Startet die Unit beim Boot, bevor das WLAN eine Adresse hat, sind
    das die einzigen Eintraege. Wuerde man dann INPUT auf DROP setzen, waere der
    Host fuer jedes LAN-Geraet gesperrt, sobald das Netz hochkommt - bis der
    Dispatcher oder der Timer nachzieht. Deshalb: erst sperren, wenn mindestens
    ein echtes LAN-Praefix bekannt ist.
    """
    import ipaddress
    for c in cidrs or []:
        net = ipaddress.ip_network(c, strict=False)
        if net.is_loopback or net.is_link_local:
            continue
        return True
    return False


def rollback(runner=_run):
    """Kette und Sprung entfernen, Policy zurueck auf ACCEPT (Notausgang)."""
    changes = []
    for ipt in (IPTABLES, IP6TABLES):
        runner([ipt, "-D", "INPUT", "-j", CHAIN])
        runner([ipt, "-F", CHAIN])
        runner([ipt, "-X", CHAIN])
        if runner([ipt, "-P", "INPUT", "ACCEPT"]).returncode == 0:
            changes.append("INPUT-Policy zurueck auf ACCEPT")
    return changes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--print", dest="show", action="store_true",
                    help="Regeln nur ausgeben")
    ap.add_argument("--dry-run", action="store_true", help="nichts anwenden")
    ap.add_argument("--status", action="store_true", help="Policy anzeigen")
    ap.add_argument("--rollback", action="store_true", help="Firewall entfernen")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    if a.rollback:
        for c in rollback():
            print("fw_rules: " + c)
        return 0

    cfg = load_defaults()

    if a.status:
        for fam, ipt in (("v4", IPTABLES), ("v6", IP6TABLES)):
            line = (_run([ipt, "-S", "INPUT"]).stdout.splitlines() or ["?"])[0]
            print("%s  %s" % (fam, line))
        return 0

    try:
        lan_v4, lan_v6 = lan_cidrs(cfg)
    except (OSError, ValueError) as e:
        print("fw_rules: 'ip -j addr show' fehlgeschlagen: %s" % e, file=sys.stderr)
        return 1

    rules4, rules6 = build_v4(cfg, lan_v4), build_v6(cfg, lan_v6)

    if a.show:
        for fam, rules in (("v4", rules4), ("v6", rules6)):
            print("# %s" % fam)
            for r in rules:
                print("  -A %s %s" % (CHAIN, " ".join(r)))
        return 0

    # Ohne echtes LAN-Netz waere DROP ein Selbstaussperrer - lieber nichts tun.
    # (Loopback/link-local zaehlen NICHT: die sind beim Boot schon da, bevor das
    # WLAN eine Adresse hat.)
    # Exit 0, NICHT 1: beim Boot laeuft der Dienst zwangslaeufig VOR dem DHCP
    # (network-online.target ist ohne NetworkManager-wait-online sofort
    # "erreicht"). Das fehlende Praefix ist dort der Normalfall und kein
    # Fehler - mit Exit 1 stand bei JEDEM Boot ein rotes "Failed" im Journal,
    # in dem ein echter Fehler untergeht. Timer und NM-Dispatcher ziehen die
    # Regeln kurz darauf nach.
    if not has_real_lan(lan_v4 + lan_v6):
        print("fw_rules: noch kein LAN-Praefix bekannt - Policy bleibt ACCEPT",
              file=sys.stderr)
        return 0

    if a.dry_run:
        print("fw_rules: wuerde %d v4- und %d v6-Regeln setzen, INPUT=DROP"
              % (len(rules4), len(rules6)))
        return 0

    try:
        lock = open(LOCK, "w")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return 0

    changes, failures = [], []
    c4, f4 = ensure_chain(IPTABLES, CHAIN, rules4)
    c6, f6 = ensure_chain(IP6TABLES, CHAIN, rules6)
    changes += c4 + c6
    failures += f4 + f6
    if failures:
        print("fw_rules: Regelanwendung fehlgeschlagen (%s) - Policy bleibt ACCEPT" % "; ".join(failures), file=sys.stderr)
        return 1

    # Erst pruefen, dann sperren. Fehlt etwas, bleibt INPUT offen statt zu.
    for ipt, rules, fam in ((IPTABLES, rules4, "v4"), (IP6TABLES, rules6, "v6")):
        problems = verify(ipt, CHAIN, rules)
        if problems:
            print("fw_rules: %s unvollstaendig (%s) - Policy bleibt ACCEPT"
                  % (fam, ", ".join(problems)), file=sys.stderr)
            return 1
        set_policy(ipt, "DROP")

    if not a.quiet:
        print("fw_rules: %s (%d LAN-Netze)"
              % ("; ".join(changes) if changes else "unveraendert",
                 len(lan_v4) + len(lan_v6)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
