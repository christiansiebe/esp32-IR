#!/usr/bin/env python3
"""Persistentes Boot-Journal mit Reset-Grund.

Warum: Die Amlogic-Hardware liefert KEINEN Reset-Grund (`watchdog/bootstatus`
ist immer 0, pstore bleibt leer). Das Journal der vorigen Laeufe bricht bei
jedem Hard-Reset abrupt ab (kein "Reached target Shutdown"), es fehlt also auch
dort der Grund. Dieses Werkzeug zaehlt die Boots persistent und leitet den Grund
aus drei Spuren ab:

  1. Clean-Marker  - `ExecStop` dieses Dienstes setzt ihn. Bei einem sauberen
     Reboot/Shutdown laeuft er, bei Hard-Reset/Watchdog/Panic nie.
  2. watchdog/bootstatus - auf Amlogic immer 0, aber portabel mitgenommen.
  3. "Kernel panic" im Kernel-Log des vorigen Boots.
  Fehlt alles und war der vorige Lauf kurz -> early-reset, sonst
  hard-reset-unknown.

Aufrufe:
  boot_journal.py start       (systemd ExecStart, beim Boot)
  boot_journal.py heartbeat   (Timer, jede Minute: letzte bekannte Uptime)
  boot_journal.py mark-clean  (systemd ExecStop: sauberer Abschied)
  boot_journal.py show        (Bericht)
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_journal_lib import (  # noqa: E402
    classify_reset, extract_panic_summary, load_state, record_boot, save_state,
    summarize,
)

STATE_DIR = os.environ.get("BOOT_JOURNAL_DIR", "/var/lib/boot-journal")
STATE_PATH = os.path.join(STATE_DIR, "state.json")
WATCHDOG_BOOTSTATUS = "/sys/class/watchdog/watchdog0/bootstatus"
# systemd-pstore.service archiviert /sys/fs/pstore beim Boot hierher und leert
# das sysfs-Verzeichnis; beide Orte werden gelesen. Ueber die Umgebungsvariable
# BOOT_JOURNAL_PSTORE_DIRS (doppelpunktgetrennt) fuer Tests ersetzbar.
PSTORE_DIRS = tuple(
    p for p in os.environ.get(
        "BOOT_JOURNAL_PSTORE_DIRS",
        "/sys/fs/pstore:/var/lib/systemd/pstore").split(":") if p)
# Ein pstore-Dump gilt nur dann als Spur DIESES Resets, wenn er frisch ist:
# ramoops behaelt den letzten Dump, bis ein neuer ihn ueberschreibt. Ohne
# Altersgrenze wuerde ein alter Panic-Dump jedem spaeteren Boot als Grund
# angehaengt. 15 min deckt den Reboot nach einem Panic bequem ab.
PSTORE_MAX_AGE_S = 900.0


def _run(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True,
                              text=True, timeout=20).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def current_boot_id():
    bid = _run("cat /proc/sys/kernel/random/boot_id")
    return bid or "unknown"


def uptime_s():
    try:
        with open("/proc/uptime", "r", encoding="utf-8") as fh:
            return float(fh.read().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def read_watchdog_bootstatus():
    try:
        with open(WATCHDOG_BOOTSTATUS, "r", encoding="utf-8") as fh:
            val = fh.read().strip()
        return val not in ("", "0")
    except OSError:
        return False


def prev_kernel_panic():
    """Sucht 'Kernel panic' im Kernel-Log des vorigen Boots (-1)."""
    out = _run("journalctl -b -1 -k --no-pager 2>/dev/null | grep -c 'Kernel panic'")
    try:
        return int(out) > 0
    except ValueError:
        return False


def find_pstore_panic(now=None, dirs=None, max_age=PSTORE_MAX_AGE_S,
                      exclude_mtime=None):
    """Sucht eine frische, noch nicht verbrauchte Panic-Spur in pstore/ramoops.

    Rueckgabe: (mtime, Kurztext) oder (None, None).

    Zwei Filter:
      * `max_age` - nur junge Dumps zaehlen.
      * `exclude_mtime` - alles, was NICHT juenger ist als diese Marke, gilt als
        bereits verbraucht. ramoops (und systemd-pstore) behalten alte Dumps;
        ohne diesen Filter wuerde ein alter Panic jedem weiteren Boot erneut als
        Grund angehaengt. Die Marke ist monoton, ein neuer Panic hat immer eine
        groessere mtime.
    """
    if now is None:
        now = time.time()
    if dirs is None:
        dirs = PSTORE_DIRS
    best = None
    for d in dirs:
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for name in names:
            path = os.path.join(d, name)
            try:
                mtime = os.path.getmtime(path)
                if now - mtime > max_age:
                    continue
                if exclude_mtime is not None and mtime <= exclude_mtime:
                    continue
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    summary = extract_panic_summary(fh.read())
            except OSError:
                continue
            if summary and (best is None or mtime > best[0]):
                best = (mtime, summary)
    return best if best else (None, None)


def pstore_panic(now=None, dirs=None, max_age=PSTORE_MAX_AGE_S):
    """Kurzform von find_pstore_panic: liefert nur den Panic-Text (oder None)."""
    return find_pstore_panic(now=now, dirs=dirs, max_age=max_age)[1]


def kernel_release():
    return _run("uname -r")


def cmd_start(args):
    state = load_state(STATE_PATH)
    prev = state.get("last")

    clean_marker = bool(state.get("clean_shutdown"))
    watchdog = read_watchdog_bootstatus()
    # Die mtime des Dumps, den ein frueherer Boot schon zugeordnet hat, wird
    # ausgeschlossen - sonst haengt ramoops denselben Panic jedem weiteren Boot an.
    consumed = state.get("consumed_pstore_mtime")
    if consumed is None and prev:
        consumed = prev.get("pstore_mtime")
    pstore_mtime, pstore_summary = find_pstore_panic(exclude_mtime=consumed)
    panic = prev_kernel_panic()
    # Letzte bekannte Uptime vor dem Reset (Heartbeat) - Fallback: Clean-Marker.
    prev_uptime = state.get("last_seen_uptime_s")
    if prev_uptime is None:
        prev_uptime = state.get("clean_shutdown_uptime_s")

    if prev is None and not clean_marker:
        reason = "first-boot"
    else:
        reason = classify_reset(clean_marker=clean_marker,
                                watchdog_bootstatus=watchdog,
                                kernel_panic=panic,
                                prev_uptime_s=prev_uptime,
                                pstore_panic=pstore_summary)

    up = uptime_s() or 0.0
    # Nur bei echtem Panic den Text uebernehmen; die mtime aber immer verbrauchen,
    # damit ein alter Dump nicht spaeter faelschlich erneut zugeordnet wird.
    state = record_boot(state, boot_id=current_boot_id(),
                        start_epoch=int(time.time()), reason=reason,
                        prev_uptime_s=prev_uptime, uptime_at_boot_s=up,
                        kernel=kernel_release(),
                        panic_summary=(pstore_summary
                                       if reason == "kernel-panic" else None),
                        pstore_mtime=pstore_mtime)
    # Marker verbrauchen und Heartbeat neu aufsetzen.
    state["clean_shutdown"] = False
    state["clean_shutdown_uptime_s"] = None
    # Verbrauchte pstore-mtime sticky und MONOTON halten: ein alter Dump darf nie
    # wieder (auch nicht nach einem sauberen Reboot) einem spaeteren Boot
    # zugeordnet werden. Neue Panics haben immer eine groessere mtime.
    if pstore_mtime is not None:
        prev_consumed = state.get("consumed_pstore_mtime") or 0
        state["consumed_pstore_mtime"] = max(prev_consumed, pstore_mtime)
    state["last_seen_uptime_s"] = up
    state["last_seen_epoch"] = int(time.time())
    save_state(STATE_PATH, state)
    print("[boot-journal] boot #%d  reason=%s  prev_uptime=%s%s" % (
        state["boots_total"], reason,
        ("%.1fs" % prev_uptime) if prev_uptime is not None else "-",
        ("  panic=%s" % pstore_summary) if pstore_summary else ""))
    return 0


def cmd_heartbeat(args):
    state = load_state(STATE_PATH)
    up = uptime_s()
    if up is None:
        return 0
    state["last_seen_uptime_s"] = up
    state["last_seen_epoch"] = int(time.time())
    save_state(STATE_PATH, state)
    return 0


def cmd_mark_clean(args):
    state = load_state(STATE_PATH)
    state["clean_shutdown"] = True
    state["clean_shutdown_uptime_s"] = uptime_s()
    save_state(STATE_PATH, state)
    print("[boot-journal] clean shutdown marker gesetzt")
    return 0


def cmd_show(args):
    state = load_state(STATE_PATH)
    print(summarize(state))
    return 0


def main():
    ap = argparse.ArgumentParser(description="persistentes Boot-Journal")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("start").set_defaults(func=cmd_start)
    sub.add_parser("heartbeat").set_defaults(func=cmd_heartbeat)
    sub.add_parser("mark-clean").set_defaults(func=cmd_mark_clean)
    sub.add_parser("show").set_defaults(func=cmd_show)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
