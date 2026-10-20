#!/usr/bin/env python3
"""Host test for the print-server health classifier (no board, no docker).

Every "blank page" failure mode of the Brother HL-3040CN print server looks the
same from outside, so the value of this module is the mapping: one signal set ->
one cause. These checks pin that mapping down, so a classifier edit cannot
silently mislabel a fault.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "brother-hl3040cn"))

from print_health import classify  # noqa: E402

checks = 0
fails = 0


def ok(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL %s" % what)


# A fully healthy chain: the classifier must NOT invent a fault.
healthy = classify({
    "container_running": True,
    "binfmt_ok": True,
    "filter_present": True,
    "ppd_present": True,
    "queue_state": "idle",
    "accepting": True,
    "printer_reachable": True,
    "cups_log": "",
})
ok("no fault" in healthy["verdict"], "healthy chain -> no fault verdict")
ok(healthy["fixes"], "healthy chain still suggests a next step")

# Container down: the classic "spinning" symptom (jobs queue, nothing prints).
r = classify({"container_running": False})
ok("DOWN" in r["verdict"], "container down detected")
ok(any("brother-cups" in f for f in r["fixes"]), "container fix mentions the name")

# qemu-i386 lost after a reboot -> i386 filter cannot run -> blank page.
r = classify({"container_running": True, "binfmt_ok": False})
ok("qemu-i386" in r["verdict"], "missing binfmt detected")
ok(any("binfmt" in f for f in r["fixes"]), "binfmt fix present")

# Filter missing in the image.
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": False})
ok("filter" in r["verdict"].lower(), "missing filter detected")

# PPD missing -> generic driver -> blank.
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": True,
              "ppd_present": False})
ok("PPD" in r["verdict"], "missing ppd detected")

# CUPS log: exec format error is the smoking gun for a lost qemu-i386.
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": True,
              "ppd_present": True,
              "cups_log": "E [..] [Job 1] Unable to execute filter: Exec format error"})
ok("executable" in r["verdict"].lower() or "exec" in r["verdict"].lower(),
   "exec format error mapped to a filter-exec cause")

# CUPS log: filter failed.
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": True,
              "ppd_present": True, "cups_log": "E [..] [Job 1] Filter failed"})
ok("filter" in r["verdict"].lower(), "filter failed mapped")

# Printer unreachable -> job stays queued (spinning).
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": True,
              "ppd_present": True, "printer_reachable": False})
ok("unreachable" in r["verdict"], "printer unreachable detected")

# Queue disabled.
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": True,
              "ppd_present": True, "printer_reachable": True, "queue_state": "disabled"})
ok("disabled" in r["verdict"] or "not accepting" in r["verdict"],
   "disabled queue detected")

# Empty stream for the last job -> blank page.
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": True,
              "ppd_present": True, "printer_reachable": True,
              "last_stream_bytes": 0})
ok("no data" in r["verdict"], "empty stream detected")

# Non-PJL stream (raw job bypassing the filter) -> blank page.
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": True,
              "ppd_present": True, "printer_reachable": True,
              "last_stream_bytes": 500000, "last_stream_pjl": False})
ok("NOT a Brother stream" in r["verdict"], "non-PJL stream detected")
ok(any("9100" in f for f in r["fixes"]), "raw-port fix mentioned")

# A real Brother stream is healthy.
r = classify({"container_running": True, "binfmt_ok": True, "filter_present": True,
              "ppd_present": True, "printer_reachable": True,
              "last_stream_bytes": 112587, "last_stream_pjl": True})
ok("no fault" in r["verdict"], "real Brother stream -> no fault")

# Every branch must return the three keys with a non-empty verdict/cause.
for sig in [{}, {"container_running": False}, {"binfmt_ok": False},
            {"cups_log": "filter failed"}]:
    r = classify(sig)
    ok(set(("verdict", "cause", "fixes")) <= set(r), "result has all keys")
    ok(bool(r["verdict"]) and bool(r["cause"]), "verdict and cause are non-empty")

print("test_print_health: %d checks, %d failures" % (checks, fails))
sys.exit(1 if fails else 0)
