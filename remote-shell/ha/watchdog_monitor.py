#!/usr/bin/env python3
"""Externer Watchdog-Monitor fuer ein Board mit ausbleibendem HW-Watchdog.

Hintergrund: `WATCHDOG-GAP.md`. Der interne systemd-/Meson-Watchdog hat einen
PLL-Hang bei 2532 MHz NICHT in 60 s rebootet (66-min-Journal-Luecke). Ein
zweiter Software-Watchdog auf derselben CPU scheitert gleich. Dieser Monitor
laeuft deshalb **ausserhalb** des Boards: er beobachtet einen Heartbeat, den
das Board sendet, und loest bei Ausbleiben einen **unabhaengigen** Aktor aus
(Power-Cut/Reset-Leitung, ueber `--reboot-cmd`).

Die Entscheidung selbst ist eine **reine Funktion** (`decide`), damit sie ohne
Board, ohne Netz und ohne Mocks host-getestet werden kann
(`test_watchdog_monitor.py`).

Heartbeat-Format (eine Zeile, `watchdog-heartbeat.sh` schreibt es):

    <empfangs-zeit_nicht_noetig> <boot_id> <seq>

Der Monitor stempelt den **Empfang** selbst (`time.time()`), nicht der Sender:
eine falsche Board-Uhr darf keinen Fehlalarm erzeugen.

Aufruf:
    python3 watchdog_monitor.py --heartbeat /run/board-heartbeat \\
        --reboot-cmd 'ssh powercut "cycle"' --interval 60 --grace 30
"""
import argparse
import os
import subprocess
import sys
import time

DEFAULTS = {
    "interval": 60.0,   # erwarteter Heartbeat-Abstand [s]
    "grace": 30.0,      # zusaetzliche Nachsicht, bevor "vermisst" zaehlt
    "miss_needed": 3,   # so viele Fehlschlaege in Folge vor einem Reboot
    "cooldown": 600.0,  # Mindestabstand zweier Rebools [s]
    "poll": 5.0,        # Abfrageintervall [s]
}


def parse_heartbeat(text):
    """Zerlegt die letzte Heartbeat-Zeile.

    Erwartet '<boot_id> <seq>' (oder '<epoch> <boot_id> <seq>', dann wird der
    erste Wert verworfen). Gibt {'boot_id': str, 'seq': int} zurueck oder None.
    """
    if not text:
        return None
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if not lines:
        return None
    parts = lines[-1].split()
    if len(parts) >= 3:          # <epoch> <boot_id> <seq>
        boot_id, seq_s = parts[1], parts[2]
    elif len(parts) == 2:        # <boot_id> <seq>
        boot_id, seq_s = parts[0], parts[1]
    else:
        return None
    try:
        seq = int(seq_s)
    except ValueError:
        return None
    return {"boot_id": boot_id, "seq": seq}


def is_stale(now, last_seen, interval, grace):
    """True, wenn der Heartbeat laenger als interval+grace ausbleibt."""
    if last_seen is None:
        return True
    return (now - last_seen) > (interval + grace)


def decide(now, last_seen, boot_id, last_boot_id, miss_count,
           interval=None, grace=None, miss_needed=None, cooldown=None,
           last_reboot=None):
    """Reine Entscheidungsfunktion.

    Eingaben sind nur Zahlen/IDs -> vollstaendig host-testbar. Gibt ein dict:

        {"action": "ok"|"miss"|"reboot"|"cooldown",
         "miss_count": int,
         "boot_id": str|None,
         "reason": str}

    Regeln:
      - kein Heartbeat / zu alt        -> miss_count+1
      - miss_count < miss_needed       -> "miss"
      - sonst, Cooldown aktiv          -> "cooldown" (kein Reboot)
      - sonst                          -> "reboot"
      - frischer Heartbeat             -> "ok", miss_count=0
      - neue boot_id                   -> "ok" (das Board hat selbst rebootet)
    """
    interval = DEFAULTS["interval"] if interval is None else interval
    grace = DEFAULTS["grace"] if grace is None else grace
    miss_needed = DEFAULTS["miss_needed"] if miss_needed is None else miss_needed
    cooldown = DEFAULTS["cooldown"] if cooldown is None else cooldown

    fresh = not is_stale(now, last_seen, interval, grace)
    boot_changed = (boot_id is not None and last_boot_id is not None
                    and boot_id != last_boot_id)

    if fresh or boot_changed:
        reason = "boot-id wechselte" if boot_changed else "heartbeat frisch"
        return {"action": "ok", "miss_count": 0, "boot_id": boot_id,
                "reason": reason}

    n = (miss_count or 0) + 1
    if n < miss_needed:
        return {"action": "miss", "miss_count": n, "boot_id": last_boot_id,
                "reason": "%d/%d Fehlversuche" % (n, miss_needed)}

    if last_reboot is not None and (now - last_reboot) < cooldown:
        return {"action": "cooldown", "miss_count": n, "boot_id": last_boot_id,
                "reason": "Cooldown aktiv (%.0fs)" % (now - last_reboot)}

    return {"action": "reboot", "miss_count": n, "boot_id": last_boot_id,
            "reason": "%d Fehlversuche, kein frischer Heartbeat" % n}


def _read_last_line(path):
    try:
        with open(path, "r") as fh:
            return fh.read()
    except OSError:
        return ""


def run_once(state, heartbeat_path, now, cfg):
    """Ein Poll: Heartbeat lesen, `decide` aufrufen, Zustand fortschreiben.

    Gibt (state, decision) zurueck. `state` traegt last_hb/last_change/
    last_boot_id/miss_count/last_reboot. Reine Nebenwirkung hier: Datei lesen.

    Wichtig: die Datei ist zwischen zwei Emitter-Laeufen **unveraendert**. Die
    Alter richtet sich daher nach der letzten **Inhaltsaenderung** (seq/boot_id
    neu), nicht nach dem Lesezeitpunkt - sonst gaelte eine stehende Datei ewig
    als frisch.
    """
    prev_boot = state.get("last_boot_id")
    hb = parse_heartbeat(_read_last_line(heartbeat_path))
    if hb is not None and hb != state.get("last_hb"):
        state["last_hb"] = hb
        state["last_change"] = now            # Inhalt neu -> Heartbeat lebt
    if hb is not None:
        state["last_boot_id"] = hb["boot_id"]

    d = decide(
        now=now,
        last_seen=state.get("last_change"),
        boot_id=state.get("last_boot_id"),
        last_boot_id=prev_boot,
        miss_count=state.get("miss_count", 0),
        interval=cfg["interval"],
        grace=cfg["grace"],
        miss_needed=cfg["miss_needed"],
        cooldown=cfg["cooldown"],
        last_reboot=state.get("last_reboot"),
    )
    state["miss_count"] = d["miss_count"]
    if d["action"] == "reboot":
        state["last_reboot"] = now
    return state, d


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--heartbeat", default="/run/board-heartbeat")
    ap.add_argument("--reboot-cmd", default="",
                    help="Aktor: Befehl fuer den Power-Cut/Reset (leer = nur loggen)")
    ap.add_argument("--interval", type=float, default=DEFAULTS["interval"])
    ap.add_argument("--grace", type=float, default=DEFAULTS["grace"])
    ap.add_argument("--miss-needed", type=int, default=DEFAULTS["miss_needed"])
    ap.add_argument("--cooldown", type=float, default=DEFAULTS["cooldown"])
    ap.add_argument("--poll", type=float, default=DEFAULTS["poll"])
    ap.add_argument("--once", action="store_true",
                    help="nur einen Poll machen (fuer Tests/Cron)")
    args = ap.parse_args(argv)

    cfg = {"interval": args.interval, "grace": args.grace,
           "miss_needed": args.miss_needed, "cooldown": args.cooldown}
    state = {}

    def log(msg):
        print("%s %s" % (time.strftime("%Y-%m-%dT%H:%M:%S"), msg), flush=True)

    while True:
        now = time.time()
        state, d = run_once(state, args.heartbeat, now, cfg)
        log("%s (%s)" % (d["action"], d["reason"]))
        if d["action"] == "reboot":
            if args.reboot_cmd:
                log("Aktor: %s" % args.reboot_cmd)
                try:
                    subprocess.call(args.reboot_cmd, shell=True)
                except OSError as exc:
                    log("Aktor fehlgeschlagen: %s" % exc)
            else:
                log("kein --reboot-cmd: nur Meldung, kein Reset")
        if args.once:
            return 0
        time.sleep(args.poll)


if __name__ == "__main__":
    sys.exit(main())
