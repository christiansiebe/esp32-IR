#!/usr/bin/env python3
"""Fleet watchdog: keep every esp32-IR chat agent awake while it has work.

Why this exists
---------------
Each OpenHands conversation runs in its own sandbox. A sandbox is *paused*
automatically once the conversation goes idle. When that happens the agent
cannot wake itself - nothing is running to do it. The only thing that can is
another conversation, so the agents watch each other: every agent re-arms every
other agent that still has open work, using the Cloud API.

The rule this enforces (see AGENTS.md):
  * every agent works through sub-agents (parent_conversation_id),
  * while a conversation has open tasks it must stay alive,
  * a conversation may only finish when it has no open tasks,
  * the fleet therefore never sleeps while any task is open.

Safety rails
------------
  * task_only  - never resurrect a conversation whose tasks are all done, so
                 finished agents are not billed for nothing.
  * max_age    - never resurrect a conversation idle for longer than this; it is
                 almost certainly finished and the user may not want it back.
  * max_wakes  - cap wakes per conversation per day; if the same agent dies more
                 than this, leave it alone and say so loudly (it is probably
                 genuinely broken, not asleep).
  * dry_run    - report what would happen without calling the API.

Only stdlib is used (urllib), so it runs on any sandbox without installs.
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

BASE_URL = os.environ.get("OPENHANDS_BASE_URL", "https://app.all-hands.dev")
ROOT = "/workspace/conversations"
FLEET_DIR = "/workspace/fleet"
STATE = os.path.join(FLEET_DIR, "fleet_state.json")
REGISTRY = os.path.join(FLEET_DIR, "agents.json")
LOG = os.path.join(FLEET_DIR, "fleet.log")

# The parent keeps fleet/agents.json up to date. It is the single source of
# truth for "does this agent still have work". A conversation that is not in the
# registry is only watched, never woken.
PARENT = "8c2548251ad045599fbfb6f95f3e7944"

WAKE_MESSAGE = (
    "System: Du wurdest vom Fleet Supervisor geweckt (deine Sandbox war pausiert). "
    "Arbeite an deinen offenen Aufgaben weiter. Regel: arbeite immer ueber "
    "Sub-Agenten, und beende erst, wenn keine offene Aufgabe mehr existiert. "
    "Wenn alles erledigt ist, setze alle Eintraege in deiner TASKS.json auf "
    "'done' und aktualisiere /workspace/fleet/agents.json."
)

DEFAULTS = {"task_only": True, "max_age_hours": 72, "max_wakes_per_day": 12}


def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), msg)
    print(line)
    try:
        with open(LOG, "a") as fh:
            fh.write(line + "\n")
    except OSError:
        pass


def api_key():
    key = os.environ.get("OPENHANDS_API_KEY") or os.environ.get("OPENHANDS_CLOUD_API_KEY")
    if key:
        return key
    # The long-running watchdog is started detached, where the platform-injected
    # env var is not present. Fall back to a sandbox-local, 0600 file. This stays
    # inside this sandbox and is never copied anywhere.
    keyfile = os.path.join(FLEET_DIR, ".api_key")
    try:
        with open(keyfile) as fh:
            return fh.read().strip()
    except OSError:
        return None


def api(method, path, body=None, timeout=45):
    key = api_key()
    if not key:
        raise RuntimeError("OPENHANDS_API_KEY ist nicht gesetzt")
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        BASE_URL + path,
        data=data,
        method=method,
        headers={"Authorization": "Bearer " + key,
                 "Accept": "application/json",
                 "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read().decode("utf-8", "replace")
    return json.loads(raw) if raw.strip() else {}


def load_state():
    try:
        with open(STATE) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {"conversations": {}}


def save_state(state):
    tmp = STATE + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(state, fh, indent=1, sort_keys=True)
    os.replace(tmp, STATE)


def open_tasks(path):
    """Return (open_count, total_count) for a TASKS.json, or None if absent."""
    if not path or not os.path.exists(path):
        return None
    try:
        with open(path) as fh:
            tasks = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(tasks, list):
        return None
    total = len(tasks)
    done = sum(1 for t in tasks if isinstance(t, dict) and t.get("status") == "done")
    return (total - done, total)


def conv_status(cid):
    try:
        rec = api("GET", "/api/v1/app-conversations?ids=" + cid)
    except urllib.error.HTTPError as e:
        return {"error": "HTTP %s" % e.code}
    except Exception as e:  # noqa: BLE001 - report anything, never crash the loop
        return {"error": str(e)}
    items = rec.get("items") if isinstance(rec, dict) else rec
    if not items:
        return {"error": "nicht gefunden"}
    it = items[0]
    return {
        "sandbox_id": it.get("sandbox_id"),
        "sandbox_status": it.get("sandbox_status"),
        "execution_status": it.get("execution_status"),
        "title": it.get("title"),
        "updated_at": it.get("updated_at"),
        "pr_number": it.get("pr_number") or [],
    }


def hours_since(iso):
    if not iso:
        return None
    try:
        from datetime import datetime, timezone
        ts = datetime.fromisoformat(iso.replace("Z", "+00:00"))
        return (datetime.now(timezone.utc) - ts).total_seconds() / 3600.0
    except ValueError:
        return None


def discover():
    """Map conversation id -> registry entry (or None) for every known worker."""
    found = {}
    try:
        with open(REGISTRY) as fh:
            reg = json.load(fh)
        agents = reg.get("agents", {}) if isinstance(reg, dict) else {}
    except (OSError, ValueError):
        agents = {}
    for cid, entry in agents.items():
        found[cid] = entry if isinstance(entry, dict) else {}
    return found


def wake(cid, info, reason, state, dry, entry):
    sandbox = info.get("sandbox_id")
    if not sandbox:
        log("  %s: kein sandbox_id - uebersprungen" % cid[:8])
        return False
    if dry:
        log("  %s: [dry-run] wuerde resume sandbox %s (%s)" % (cid[:8], sandbox, reason))
        return True
    try:
        api("POST", "/api/v1/sandboxes/%s/resume" % sandbox, {})
        # Nudge the agent with its open work, otherwise it wakes idle.
        try:
            api("POST", "/api/v1/app-conversations/%s/send-message" % cid,
                {"role": "user",
                 "content": [{"type": "text", "text": WAKE_MESSAGE}],
                 "run": True})
        except Exception as e:  # noqa: BLE001 - resume already succeeded
            log("  %s: Weck-Nachricht fehlgeschlagen: %s" % (cid[:8], e))
        rec = state["conversations"].setdefault(cid, {})
        rec["wakes"] = rec.get("wakes", 0) + 1
        rec["last_wake"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        log("  %s: resumed sandbox %s (%s) - wake #%d" % (cid[:8], sandbox, reason, rec["wakes"]))
        return True
    except urllib.error.HTTPError as e:
        log("  %s: resume fehlgeschlagen HTTP %s" % (cid[:8], e.code))
        return False
    except Exception as e:  # noqa: BLE001
        log("  %s: resume fehlgeschlagen: %s" % (cid[:8], e))
        return False


def run_once(cfg, dry):
    state = load_state()
    today = time.strftime("%Y-%m-%d", time.gmtime())
    targets = discover()
    log("=== fleet cycle: %d Gespraeche ===" % len(targets))
    woken = 0
    for cid, entry in targets.items():
        name = (entry or {}).get("name") or cid[:8]
        open_n = (entry or {}).get("open_tasks")
        if open_n is None:
            open_n = None
        info = conv_status(cid)
        if "error" in info:
            log("  %s (%s): Statusfehler: %s" % (cid[:8], name, info["error"]))
            continue
        status = info.get("sandbox_status")
        idle_h = hours_since(info.get("updated_at"))

        # the parent hosts this watchdog; never wake it from itself
        if cid == PARENT:
            log("  %s (%s): Parent/Watchdog-Host - nur beobachtet" % (cid[:8], name))
            continue
        # rule: only wake conversations that still have open work
        if cfg["task_only"] and open_n == 0:
            log("  %s (%s): alle Aufgaben erledigt - schlafen lassen" % (cid[:8], name))
            continue
        # rule: do not resurrect ancient, surely-finished agents
        if idle_h is not None and idle_h > cfg["max_age_hours"]:
            log("  %s (%s): seit %.1fh inaktiv (> %.0fh) - schlafen lassen"
                % (cid[:8], name, idle_h, cfg["max_age_hours"]))
            continue
        # rule: cap wakes so a genuinely broken agent is not retried forever
        rec = state["conversations"].setdefault(cid, {})
        if rec.get("day") != today:
            rec["day"] = today
            rec["wakes"] = 0
        if rec.get("wakes", 0) >= cfg["max_wakes_per_day"]:
            log("  %s (%s): %d Wakes heute erreicht - uebersprungen (evtl. defekt)"
                % (cid[:8], name, rec["wakes"]))
            continue

        if status == "PAUSED":
            reason = "Aufgaben offen (%s)" % open_n
            if wake(cid, info, reason, state, dry, entry):
                woken += 1
        elif status == "RUNNING":
            log("  %s (%s): laeuft (%s, offene Aufgaben=%s) - ok"
                % (cid[:8], name, info.get("execution_status"), open_n))
        else:
            log("  %s (%s): Status %s - nichts zu tun" % (cid[:8], name, status))
    save_state(state)
    log("=== fleet cycle fertig: %d aufgeweckt ===" % woken)
    return woken


def main():
    ap = argparse.ArgumentParser(description="esp32-IR fleet watchdog")
    ap.add_argument("--loop", action="store_true", help="dauerhaft laufen")
    ap.add_argument("--interval", type=int, default=300, help="Sekunden zwischen Zyklen")
    ap.add_argument("--dry-run", action="store_true", help="nur berichten")
    ap.add_argument("--task-only", dest="task_only", action="store_true", default=DEFAULTS["task_only"])
    ap.add_argument("--all", dest="task_only", action="store_false",
                    help="auch ohne TASKS.json aufwecken")
    ap.add_argument("--max-age-hours", type=float, default=DEFAULTS["max_age_hours"])
    ap.add_argument("--max-wakes-per-day", type=int, default=DEFAULTS["max_wakes_per_day"])
    ap.add_argument("--once", action="store_true", help="genau ein Zyklus")
    args = ap.parse_args()

    cfg = {"task_only": args.task_only,
           "max_age_hours": args.max_age_hours,
           "max_wakes_per_day": args.max_wakes_per_day}
    os.makedirs(FLEET_DIR, exist_ok=True)
    if not api_key():
        log("WARNUNG: kein OPENHANDS_API_KEY - Statusabfrage wird scheitern")

    if args.once or not args.loop:
        run_once(cfg, args.dry_run)
        return
    log("fleet watchdog gestartet (intervall=%ds, task_only=%s)" % (args.interval, cfg["task_only"]))
    while True:
        try:
            run_once(cfg, args.dry_run)
        except Exception as e:  # noqa: BLE001 - the loop must survive anything
            log("Zyklus-Fehler: %s" % e)
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
