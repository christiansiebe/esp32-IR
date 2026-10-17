#!/usr/bin/env python3
"""Host-Test fuer die reinen Entscheidungen im Kanal-Agenten.

Hintergrund: der Odroid-Agent laeuft in einer Reconnect-Schleife gegen jede
Sandbox, die je im Register stand. Eine beendete Sandbox antwortet mit HTTP
404; ohne Backoff kostet das dauerhaft CPU (gemessen 9-14 %) und hebt die Last
so weit, dass die Kerne nicht in den tiefen Takt fallen.

Getestet wird ohne Netz, ohne asyncio - nur die reinen Funktionen:
  * cmd_agent.next_delay       (Backoff, 404 = tot)
  * register_sandbox.prune_sandboxes (Register verfaellt)

    python3 remote-shell/test_channel_backoff.py
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
checks = 0
failures = 0


def check(cond, label):
    global checks, failures
    checks += 1
    if cond:
        print(f"  ok   {label}")
    else:
        failures += 1
        print(f"  FAIL {label}")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    agent = load("cmd_agent", os.path.join(HERE, "cmd_agent.py"))
    reg = load("register_sandbox", os.path.join(HERE, "register_sandbox.py"))

    print("next_delay: 404 (Sandbox tot) -> waechst bis 900 s")
    d = 5
    seq = []
    for _ in range(10):
        d = agent.next_delay(d, "server rejected WebSocket connection: HTTP 404")
        seq.append(d)
    check(seq == [10, 20, 40, 80, 160, 320, 640, 900, 900, 900],
          f"404-Sequenz {seq}")
    check(seq[-1] == 900, "404-Backoff endet bei 900 s")

    print("next_delay: normaler Fehler -> waechst nur bis 60 s")
    d = 5
    seq = []
    for _ in range(8):
        d = agent.next_delay(d, "Connection refused")
        seq.append(d)
    check(seq == [10, 20, 40, 60, 60, 60, 60, 60], f"Fehler-Sequenz {seq}")
    check(seq[-1] == 60, "Fehler-Backoff endet bei 60 s")

    print("next_delay: ein toter Eintrag kostet hoechstens ~1 Versuch/15 min")
    check(agent.next_delay(900, "HTTP 404") == 900, "900 bleibt 900 (404)")
    check(agent.next_delay(5, "x", base=1) == 10,
          "base hebt den Startwert nicht an, delay dominiert")
    check(agent.next_delay(1, "x", base=5) == 10,
          "delay unter base -> base*2")

    print("next_delay: 404 wird nur an der Zahl erkannt, nicht am Wortlaut")
    check(agent.next_delay(5, "HTTP 404") == 10, "kurzer 404-Text reicht")
    check(agent.next_delay(5, "got 4040 bytes") == 10,
          "404 als Teil einer Zahl zaehlt auch (konservativ)")

    print("prune_sandboxes: alte Eintraege fallen raus")
    now = 1_700_000_000.0
    fresh = "2023-11-14T22:13:20Z"   # == now
    old = "2023-11-13T22:13:19Z"     # 24 h + 1 s alt
    data = {"sandboxes": {
        "fresh": {"url": "u1", "updated": fresh},
        "old": {"url": "u2", "updated": old},
    }}
    reg.prune_sandboxes(data, now)
    check("fresh" in data["sandboxes"], "frischer Eintrag bleibt")
    check("old" not in data["sandboxes"], "alter Eintrag fliegt raus")

    print("prune_sandboxes: unlesbarer Zeitstempel zaehlt als alt")
    data = {"sandboxes": {"broken": {"url": "u"}}}
    reg.prune_sandboxes(data, now)
    check(data["sandboxes"] == {}, "fehlender Zeitstempel -> entfernt")

    print("prune_sandboxes: ttl ist einstellbar, paths bleiben erhalten")
    data = {"sandboxes": {"old": {"url": "u", "updated": old}}, "paths": {"memory": "m"}}
    reg.prune_sandboxes(data, now, ttl=100000)
    check("old" in data["sandboxes"], "groessere ttl behaelt den Eintrag")
    check(data["paths"] == {"memory": "m"}, "paths bleiben unberuehrt")

    print("prune_sandboxes: leeres/fehlendes sandboxes-Feld ist ok")
    d1 = {}
    reg.prune_sandboxes(d1, now)
    check(d1["sandboxes"] == {}, "fehlendes Feld -> leeres dict")

    print(f"\nchannel-backoff: {checks} Checks, {failures} Fehler")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
