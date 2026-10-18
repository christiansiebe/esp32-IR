#!/usr/bin/env python3
"""Host test for the fleet supervisor's decision table.

Runs without the Cloud API: it imports the pure decide() function and checks the
cases that caused agents to fall asleep and never wake:

  1. a worker that inherits its parent's sandbox (sandbox RUNNING, turn ended)
     must be woken - the old code keyed only on sandbox_status == PAUSED;
  2. a turn that *failed* (execution_status == "error") must be woken - the old
     code keyed only on paused/finished and let "error" sleep forever;
  3. a finished worker sharing a live sandbox must NOT pause it (that would
     freeze the parent);
  4. the coordinator is never paused;
  5. a wedged running turn is interrupted, not just woken.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.join(HERE, "..", "automations", "fleet-supervisor", "main.py")

spec = importlib.util.spec_from_file_location("fleet_supervisor", MAIN)
sup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sup)

PARENT = "7a894c13678043e58935dd1509217a75"
WORKER = "b56660b93576404d94dccf406be168c6"

fails = []


def check(name, got, want):
    if got == want:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name}: got {got!r}, want {want!r}")
        fails.append(name)


def conv(cid, sandbox="RUNNING", exec_status="running", age_min=0):
    from datetime import datetime, timedelta, timezone
    ts = (datetime.now(timezone.utc) - timedelta(minutes=age_min)).isoformat()
    return {"id": cid, "sandbox_status": sandbox, "execution_status": exec_status,
            "sandbox_id": "sbx1", "updated_at": ts}


print("fleet-supervisor decide():")
E0 = {"open_tasks": 0}
E1 = {"open_tasks": 1}

# 1. inherits parent sandbox, turn ended -> wake (not "ok", not sleep)
check("shared sandbox, turn finished -> wake",
      sup.decide(conv(WORKER, "RUNNING", "finished"), E1, {}, False), "wake")
check("shared sandbox, turn paused -> wake",
      sup.decide(conv(WORKER, "RUNNING", "paused"), E1, {}, False), "wake")

# 2. failed turn -> wake (this was the live bug: exec=error sat forever)
check("turn errored -> wake",
      sup.decide(conv(PARENT, "RUNNING", "error"), E1, {}, False), "wake")
check("no exec status yet -> wake",
      sup.decide(conv(PARENT, "RUNNING", None), E1, {}, False), "wake")

# sandbox paused -> wake
check("sandbox paused -> wake",
      sup.decide(conv(WORKER, "PAUSED", None), E1, {}, False), "wake")

# 3. genuinely running turn -> ok
check("running turn -> ok",
      sup.decide(conv(PARENT, "RUNNING", "running"), E1, {}, False), "ok")

# 4. finished worker in a shared live sandbox must not pause it
check("finished worker, shared sandbox -> skip (no pause)",
      sup.decide(conv(WORKER, "RUNNING", "finished"), E0, {}, True), "skip")
# finished worker alone -> pause
check("finished worker, sandbox free -> pause",
      sup.decide(conv(WORKER, "RUNNING", "finished"), E0, {}, False), "pause")
# the coordinator is never paused
check("coordinator never paused",
      sup.decide(conv(PARENT, "RUNNING", "finished"), E0, {}, False), "skip")

# 5. wedged running turn -> stuck (interrupt)
check("wedged turn -> stuck",
      sup.decide(conv(PARENT, "RUNNING", "running", age_min=30), E1, {}, False), "stuck")
check("just under threshold -> ok",
      sup.decide(conv(PARENT, "RUNNING", "running", age_min=1), E1, {}, False), "ok")

# rails
check("too many wakes -> skip",
      sup.decide(conv(PARENT, "RUNNING", "error"), E1, {"wakes": sup.MAX_WAKES}, False), "skip")

print()
if fails:
    print(f"FAILED: {len(fails)} check(s): {fails}")
    sys.exit(1)
print("alle Checks bestanden")
