#!/usr/bin/env python3
"""Reine Logik fuer das Boot-Journal (host-testbar, keine Board-Abhaengigkeiten).

Die Amlogic-Hardware liefert KEINEN Reset-Grund: watchdog `bootstatus` ist immer
0. Der Grund muss deshalb aus Spuren rekonstruiert werden:

  * Hat der vorige Lauf einen Clean-Marker (systemd-shutdown-Hook) hinterlassen?
    -> sauberer Reboot/Shutdown.
  * Sonst: watchdog bootstatus gesetzt? -> Watchdog-Reset.
  * Sonst: steht in einem pstore/ramoops-Dump eine Panic? -> kernel-panic
    (mit Kurztext). Das ist die verlaesslichste Spur: ramoops schreibt das
    Panic-Dmesg in reservierten RAM, der einen Warm-Reset ueberlebt.
  * Sonst: stand im vorigen Kernel-Log eine Panic? -> kernel-panic (Fallback,
    greift nur wenn journald noch geflusht hat).
  * Sonst: war der vorige Lauf sehr kurz? -> early-reset (Absturz beim Start).
  * Sonst: -> hard-reset-unknown (Hang, Watchdog, Panic oder Stromausfall).

Diese Datei haelt nur die Entscheidung + das Zustandsformat. Das Einsammeln der
Spuren (Journal, sysfs, pstore) macht `boot_journal.py`.
"""
from __future__ import annotations

import json
import os

STATE_VERSION = 1
HISTORY_MAX = 50
# Ein Lauf, der kuerzer als das hier war, gilt als "early" (Absturz beim Start).
EARLY_RESET_MAX_UPTIME_S = 60.0

# Marker, die im pstore-Dump auf einen Kernel-Absturz hindeuten. Reihenfolge
# zaehlt: die erste passende Zeile im Dump wird als Kurztext genommen.
PANIC_MARKERS = (
    "Kernel panic",
    "Unable to handle kernel",
    "Internal error:",
    "BUG: unable to handle",
    "BUG: kernel NULL pointer",
)

REASONS = (
    "first-boot",          # noch kein voriger Lauf bekannt
    "clean-reboot",        # systemd-shutdown-Hook hat den Marker gesetzt
    "watchdog-reset",      # watchdog bootstatus gesetzt (auf Amlogic ungenutzt)
    "kernel-panic",        # Panic-Spur in pstore/ramoops oder im vorigen Log
    "early-reset",         # voriger Lauf war kuerzer als EARLY_RESET_MAX_UPTIME_S
    "hard-reset-unknown",  # abrupt, ohne Spur (Hang/Panic/Stromausfall)
)


def extract_panic_summary(text, limit=200):
    """Zieht die erste Panic-Zeile aus einem pstore-Dmesg (oder None).

    dmesg-Zeilen sehen wie `<0>[   50.049214] Kernel panic - not syncing: ...`
    aus; der Level-/Zeitstempel-Praefix wird entfernt.
    """
    for line in text.splitlines():
        stripped = line.split("] ", 1)[-1] if "] " in line else line
        for marker in PANIC_MARKERS:
            if marker in stripped:
                return stripped.strip()[:limit]
    return None


def classify_reset(*, clean_marker, watchdog_bootstatus, kernel_panic,
                   prev_uptime_s, pstore_panic=None):
    """Leitet den Reset-Grund des laufenden Boots aus den Spuren ab."""
    if clean_marker:
        return "clean-reboot"
    if watchdog_bootstatus:
        return "watchdog-reset"
    if pstore_panic or kernel_panic:
        return "kernel-panic"
    if prev_uptime_s is not None and prev_uptime_s < EARLY_RESET_MAX_UPTIME_S:
        return "early-reset"
    return "hard-reset-unknown"


def default_state():
    return {"version": STATE_VERSION, "boots_total": 0, "last": None,
            "history": []}


def load_state(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            state = json.load(fh)
    except (OSError, ValueError):
        return default_state()
    if not isinstance(state, dict) or state.get("version") != STATE_VERSION:
        return default_state()
    state.setdefault("boots_total", 0)
    state.setdefault("last", None)
    state.setdefault("history", [])
    return state


def save_state(path, state):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=2, sort_keys=True)
        fh.write("\n")
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)


def record_boot(state, *, boot_id, start_epoch, reason, prev_uptime_s,
                uptime_at_boot_s, kernel="", panic_summary=None,
                pstore_mtime=None, history_max=HISTORY_MAX):
    """Traegt den laufenden Boot ein und zaehlt boots_total hoch.

    Der Eintrag beschreibt den Reset, der ZU DIESEM Boot fuehrte (`reason`) -
    genau das, was `journalctl --list-boots` nicht sagen kann.
    """
    entry = {
        "boot_id": boot_id,
        "start_epoch": start_epoch,
        "reason": reason,
        "prev_uptime_s": prev_uptime_s,
        "uptime_at_boot_s": uptime_at_boot_s,
        "kernel": kernel,
    }
    if panic_summary:
        entry["panic"] = panic_summary
    if pstore_mtime is not None:
        entry["pstore_mtime"] = pstore_mtime
    state["boots_total"] = int(state.get("boots_total", 0)) + 1
    state["last"] = entry
    hist = list(state.get("history", []))
    hist.append(entry)
    if history_max and len(hist) > history_max:
        hist = hist[-history_max:]
    state["history"] = hist
    return state


def summarize(state):
    """Kurzer Textbericht fuer die Konsole."""
    lines = ["Boot-Journal: %d Boot(s) gezaehlt" % state.get("boots_total", 0)]
    last = state.get("last")
    if last:
        lines.append("  letzter Start: %s  Grund: %s" % (
            last.get("boot_id", "?"), last.get("reason", "?")))
        pu = last.get("prev_uptime_s")
        if pu is not None:
            lines.append("  voriger Lauf lief %.1f s" % pu)
        if last.get("panic"):
            lines.append("  Panic: %s" % last["panic"])
    for e in state.get("history", [])[-10:]:
        lines.append("  %s  %-20s  prev_uptime=%s" % (
            e.get("boot_id", "?")[:12], e.get("reason", "?"),
            ("%.0fs" % e["prev_uptime_s"]) if e.get("prev_uptime_s") is not None else "-"))
    return "\n".join(lines)
