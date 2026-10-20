#!/usr/bin/env python3
"""Host-Tests fuer crash_capture (Aufzeichnungs-Konfiguration + Audit)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crash_capture import (  # noqa: E402
    JOURNALD_SETTINGS, KERNEL_EXTRAARGS, SYSCTL_SETTINGS, audit,
    format_audit, merge_extraargs, render_journald_dropin,
    render_netconsole_module, render_sysctl_dropin,
)

checks = 0
fails = []


def ok(cond, msg):
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


# --- Sysctl-Drop-in ---
s = render_sysctl_dropin()
ok("kernel.panic_on_oops = 1" in s, "panic_on_oops=1 steht drin")
ok("kernel.panic = 10" in s, "panic=10 steht drin")
ok("kernel.hung_task_panic = 1" in s, "hung_task_panic=1 steht drin")
ok("vm.panic_on_oom = 0" in s, "OOM darf NICHT rebooten")
ok(s.endswith("\n"), "Datei endet mit Newline")
for key in SYSCTL_SETTINGS:
    ok(key in s, f"Sysctl {key} wird gerendert")
# sysctl.d kennt keine Inline-Kommentare: auf einer Wert-Zeile darf kein '#' stehen.
for line in s.splitlines():
    if "=" in line and not line.lstrip().startswith("#"):
        ok("#" not in line, f"kein Inline-Kommentar auf Wert-Zeile: {line!r}")

# --- journald-Drop-in ---
j = render_journald_dropin()
ok("[Journal]" in j, "journald-Drop-in hat [Journal]-Sektion")
ok("Storage=persistent" in j, "Storage=persistent")
ok("SyncIntervalSec=1s" in j, "SyncIntervalSec=1s (schnelles Flushen)")
ok("SystemMaxUse=64M" in j, "groesserer Journal-Deckel")
for key in JOURNALD_SETTINGS:
    ok(key in j, f"journald {key} wird gerendert")
for line in j.splitlines():
    if "=" in line and not line.lstrip().startswith("#"):
        ok("#" not in line, f"kein Inline-Kommentar in journald: {line!r}")

# --- merge_extraargs: ergaenzt, behaelt, keine Duplikate ---
ok(merge_extraargs("") == "panic_on_oops=1 hung_task_panic=1",
   "leere Cmdline bekommt beide Token")
ok(merge_extraargs("cpufreq.default_governor=performance")
   == "cpufreq.default_governor=performance panic_on_oops=1 hung_task_panic=1",
   "vorhandener Token bleibt, Reihenfolge erhalten")
ok(merge_extraargs("panic_on_oops=1") == "panic_on_oops=1 hung_task_panic=1",
   "vorhandenes panic_on_oops wird nicht verdoppelt")
ok(merge_extraargs("panic_on_oops=1 hung_task_panic=1")
   == "panic_on_oops=1 hung_task_panic=1", "idempotent")
ok(merge_extraargs("a=1  b=2") == "a=1 b=2 panic_on_oops=1 hung_task_panic=1",
   "Mehrfach-Leerzeichen normalisiert")
for token in KERNEL_EXTRAARGS:
    ok(token in merge_extraargs(""), f"Cmdline-Token {token} wird ergaenzt")

# --- netconsole ---
n = render_netconsole_module("192.168.178.71", "eth0", "192.168.178.64",
                             "aa:bb:cc:dd:ee:ff")
ok("options netconsole netconsole=6666@192.168.178.71/eth0,6666@192.168.178.64/aa:bb:cc:dd:ee:ff" in n,
   "netconsole-Zeile korrekt gerendert")
n2 = render_netconsole_module("", "", "", "")
ok("<src-ip>" in n2 and "<dst-ip>" in n2, "leere Werte -> Platzhalter")

# --- audit: alles gut -> keine Luecke ---
good = {
    "sysctl": {k: v[0] for k, v in SYSCTL_SETTINGS.items()},
    "journald": {k: v[0] for k, v in JOURNALD_SETTINGS.items()},
    "extraargs": "panic_on_oops=1 hung_task_panic=1",
    "ramoops_loaded": True,
}
ok(audit(good) == [], "vollstaendige Konfiguration -> keine Luecke")
ok("OK" in format_audit([]), "format_audit meldet OK")

# --- audit: leere Konfiguration -> alle Luecken ---
empty = {"sysctl": {}, "journald": {}, "extraargs": "", "ramoops_loaded": False}
gaps = audit(empty)
names = {g["name"] for g in gaps}
ok("kernel.panic_on_oops" in names, "fehlendes panic_on_oops wird gemeldet")
ok("Storage" in names, "fehlendes journald Storage wird gemeldet")
ok("SyncIntervalSec" in names, "fehlendes SyncIntervalSec wird gemeldet")
ok("panic_on_oops=1" in names, "fehlender Cmdline-Token wird gemeldet")
ok("ramoops" in names, "fehlendes ramoops wird gemeldet")
ok(all("fix" in g and "detail" in g for g in gaps),
   "jede Luecke hat detail+fix")
ok(len(format_audit(gaps)) > 0, "format_audit liefert Text")

# --- audit: ein einzelner Fehler wird genau einmal gemeldet ---
one = dict(good)
one["sysctl"] = dict(good["sysctl"])
one["sysctl"]["kernel.panic_on_oops"] = "0"
gaps = audit(one)
ok(len(gaps) == 1 and gaps[0]["name"] == "kernel.panic_on_oops",
   "genau die eine Abweichung wird gemeldet")
ok(gaps[0]["fix"] == "kernel.panic_on_oops=1", "Fix nennt den Zielwert")

# --- audit: SyncIntervalSec nur melden wenn TRAEGER als gewuenscht ---
slow = dict(good)
slow["journald"] = dict(good["journald"])
slow["journald"]["SyncIntervalSec"] = "5min"
ok(any(g["name"] == "SyncIntervalSec" for g in audit(slow)),
   "traegeres Sync wird gemeldet")
fast = dict(good)
fast["journald"] = dict(good["journald"])
fast["journald"]["SyncIntervalSec"] = "1s"
ok(not any(g["name"] == "SyncIntervalSec" for g in audit(fast)),
   "gleich schnelles Sync wird NICHT gemeldet")

# --- audit: panic=0 ist eine eigene Luecke ---
noreset = dict(good)
noreset["sysctl"] = dict(good["sysctl"])
noreset["sysctl"]["kernel.panic"] = "0"
ok(any(g["name"] == "kernel.panic" for g in audit(noreset)),
   "panic=0 wird als Luecke erkannt")

print(f"{checks} Checks, {len(fails)} Fehler")
for f in fails:
    print("  FAIL:", f)
raise SystemExit(1 if fails else 0)
