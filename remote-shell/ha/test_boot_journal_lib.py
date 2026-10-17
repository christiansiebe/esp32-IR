#!/usr/bin/env python3
"""Host-Tests fuer boot_journal_lib (Reset-Grund-Klassifikation + Zustand)."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boot_journal_lib import (  # noqa: E402
    classify_reset, default_state, extract_panic_summary, load_state,
    record_boot, save_state, summarize, EARLY_RESET_MAX_UPTIME_S,
)

checks = 0
fails = []


def ok(cond, msg):
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


# --- classify_reset: Prioritaet clean > watchdog > panic > early > unknown ---
ok(classify_reset(clean_marker=True, watchdog_bootstatus=True,
                  kernel_panic=True, prev_uptime_s=5) == "clean-reboot",
   "clean schlaegt alles")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=True,
                  kernel_panic=True, prev_uptime_s=5) == "watchdog-reset",
   "watchdog schlaegt panic/early")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=False,
                  kernel_panic=True, prev_uptime_s=5) == "kernel-panic",
   "panic schlaegt early")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=False,
                  kernel_panic=False, prev_uptime_s=5) == "early-reset",
   "kurzer Lauf -> early-reset")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=False,
                  kernel_panic=False, prev_uptime_s=3600) == "hard-reset-unknown",
   "langer Lauf ohne Marker -> hard-reset-unknown")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=False,
                  kernel_panic=False, prev_uptime_s=None) == "hard-reset-unknown",
   "ohne prev_uptime -> hard-reset-unknown")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=False,
                  kernel_panic=False,
                  prev_uptime_s=EARLY_RESET_MAX_UPTIME_S) == "hard-reset-unknown",
   "genau an der Grenze -> nicht early")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=False,
                  kernel_panic=False,
                  prev_uptime_s=EARLY_RESET_MAX_UPTIME_S - 0.1) == "early-reset",
   "knapp unter der Grenze -> early")

# --- pstore-Panic schlaegt early, aber nicht clean/watchdog ---
ok(classify_reset(clean_marker=False, watchdog_bootstatus=False,
                  kernel_panic=False, prev_uptime_s=5,
                  pstore_panic="Kernel panic - not syncing: x") == "kernel-panic",
   "pstore-Panic schlaegt early-reset")
ok(classify_reset(clean_marker=True, watchdog_bootstatus=False,
                  kernel_panic=False, prev_uptime_s=5,
                  pstore_panic="Kernel panic") == "clean-reboot",
   "clean schlaegt pstore-Panic")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=True,
                  kernel_panic=False, prev_uptime_s=5,
                  pstore_panic="Kernel panic") == "watchdog-reset",
   "watchdog schlaegt pstore-Panic")
ok(classify_reset(clean_marker=False, watchdog_bootstatus=False,
                  kernel_panic=False, prev_uptime_s=3600,
                  pstore_panic=None) == "hard-reset-unknown",
   "ohne pstore-Panic -> hard-reset-unknown")

# --- extract_panic_summary ---
SAMPLE = "\n".join([
    "<6>[    0.000000] Booting Linux on physical CPU 0x0",
    "<4>[   50.070434] Call trace:",
    "<0>[   50.049214] Kernel panic - not syncing: sysrq triggered crash",
    "<4>[   50.080139]  dump_stack+0x18/0x24",
])
ok(extract_panic_summary(SAMPLE) ==
   "Kernel panic - not syncing: sysrq triggered crash",
   "extract_panic_summary findet die Panic-Zeile ohne Praefix")
ok(extract_panic_summary("nur harmloses Zeug\n") is None,
   "extract_panic_summary: keine Panic -> None")
ok(extract_panic_summary("<4>[  1.0] Unable to handle kernel NULL pointer") ==
   "Unable to handle kernel NULL pointer", "Ooops-Marker erkannt")
ok(extract_panic_summary("<2>[  2.0] Internal error: Oops") ==
   "Internal error: Oops", "Internal-error-Marker erkannt")
ok(extract_panic_summary("") is None, "leerer Text -> None")
long_line = "Kernel panic - not syncing: " + "x" * 500
ok(len(extract_panic_summary(long_line)) == 200, "Panic-Text auf 200 gekappt")

# --- Zustand: Defaults, Round-Trip, Zaehler ---
st = default_state()
ok(st["boots_total"] == 0 and st["history"] == [] and st["last"] is None,
   "default_state leer")

with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, "sub", "state.json")
    ok(load_state(p)["boots_total"] == 0, "fehlende Datei -> Default")
    st = default_state()
    st = record_boot(st, boot_id="aaa", start_epoch=100, reason="first-boot",
                     prev_uptime_s=None, uptime_at_boot_s=1.0)
    save_state(p, st)
    back = load_state(p)
    ok(back["boots_total"] == 1, "Round-Trip Zaehler")
    ok(back["last"]["boot_id"] == "aaa", "Round-Trip last")
    ok(back["last"]["reason"] == "first-boot", "Round-Trip reason")
    # Unterordner wurde angelegt?
    ok(os.path.isdir(os.path.dirname(p)), "Elternordner angelegt")

# --- Zaehler inkrementiert monoton, History gekappt ---
st = default_state()
for i in range(60):
    st = record_boot(st, boot_id="b%d" % i, start_epoch=i, reason="x",
                     prev_uptime_s=10, uptime_at_boot_s=1.0, history_max=50)
ok(st["boots_total"] == 60, "boots_total zaehlt alle")
ok(len(st["history"]) == 50, "History auf 50 gekappt")
ok(st["history"][-1]["boot_id"] == "b59", "neuester Eintrag hinten")
ok(st["last"]["boot_id"] == "b59", "last = neuester")

# --- Kaputte/ fremde Datei -> Default, kein Absturz ---
with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, "state.json")
    with open(p, "w") as fh:
        fh.write("{ kaputt")
    ok(load_state(p)["boots_total"] == 0, "kaputtes JSON -> Default")
    with open(p, "w") as fh:
        fh.write('{"version": 999, "boots_total": 42}')
    ok(load_state(p)["boots_total"] == 0, "fremde Version -> Default")

# --- summarize darf nie werfen ---
txt = summarize(default_state())
ok("Boot-Journal" in txt, "summarize leer ok")
st = default_state()
st = record_boot(st, boot_id="zzz", start_epoch=1, reason="early-reset",
                 prev_uptime_s=4.2, uptime_at_boot_s=0.9)
ok("early-reset" in summarize(st), "summarize nennt Grund")

# --- panic_summary landet im Eintrag und im Bericht ---
st = default_state()
st = record_boot(st, boot_id="p1", start_epoch=1, reason="kernel-panic",
                 prev_uptime_s=50.0, uptime_at_boot_s=1.0,
                 panic_summary="Kernel panic - not syncing: sysrq triggered crash")
ok(st["last"].get("panic") ==
   "Kernel panic - not syncing: sysrq triggered crash", "panic im Eintrag")
ok("Panic:" in summarize(st), "summarize zeigt Panic")
# Ohne Panic kein Schluessel (schmales Zustandsformat).
st2 = default_state()
st2 = record_boot(st2, boot_id="p2", start_epoch=1, reason="clean-reboot",
                  prev_uptime_s=10.0, uptime_at_boot_s=1.0)
ok("panic" not in st2["last"], "ohne Panic kein panic-Feld")
ok("Panic:" not in summarize(st2), "summarize ohne Panic")

print("boot_journal_lib: %d Checks, %d Fehler" % (checks, len(fails)))
for f in fails:
    print("  FEHLER:", f)
sys.exit(1 if fails else 0)
