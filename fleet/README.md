# Fleet: chat agents keep each other awake

Standing rule for every esp32-IR chat agent (see `AGENTS.md`, section
"Fleet rule"): work through sub-agents, and never sleep while open work exists.

## The problem this solves

A conversation runs in its own sandbox. The platform **pauses** a sandbox once
the conversation goes idle. A paused agent cannot wake itself - nothing is
running inside it. Only another conversation can bring it back.

The user's requirement: *"all chat agents always work with sub-agents, and the
whole system keeps each other from hanging or falling asleep as long as there
are tasks."*

## How it works

```
   platform scheduler (cron */5 min)  <- outside every sandbox: survives pauses
              |
              v
   automations/fleet-watchdog/main.py   (no LLM, no token cost)
              |  reads
              v
   fleet/agents.json  (raw.githubusercontent, public repo)
              |  open_tasks per worker (0 = may sleep)
              v
   for each worker with open_tasks > 0 whose sandbox is PAUSED:
       POST /api/v1/sandboxes/<id>/resume
       POST /api/v1/app-conversations/<id>/send-message   (wake nudge)
```

The **cron automation is the guarantee** because it runs on the platform
scheduler, not inside a sandbox - a paused sandbox cannot wake itself, but the
scheduler is always running. The in-sandbox `fleet.py` loop is only a fast local
fallback and cannot be the guarantee for the same reason.

Each worker also keeps a `TASKS.json` in its own workspace with one entry per
task (`"status": "open" | "done"`). The parent mirrors the open-task count into
`fleet/agents.json`, which is the registry the watchdog trusts.

## Files

- `fleet.py` - the watchdog. Stdlib only.
- `nudge.py` - send one message to a conversation (wake nudge / manual test).
- `agents.json` - the worker registry the parent maintains.
- `fleet_state.json` - wake bookkeeping (created at runtime, not committed).
- `.api_key` - sandbox-local 0600 key file for the detached watchdog (not committed).

## Usage

```bash
export OPENHANDS_API_KEY=...          # or leave the 0600 .api_key in place
python3 fleet.py --once --dry-run     # report only
python3 fleet.py --once               # one real cycle
python3 fleet.py --loop --interval 300
```

Safety rails: `--task-only` (never wake an agent with no open tasks),
`--max-age-hours` (default 72, do not resurrect long-finished agents),
`--max-wakes-per-day` (default 12, stop retrying a genuinely broken agent and
report it instead of looping).

## Rule for a worker agent

1. Keep your `TASKS.json` accurate; `open` means "keep me awake".
2. Work through sub-agents; the parent coordinates and reviews.
3. When everything is done, set your tasks to `done`, ask the parent to update
   `agents.json`, and only then let the sandbox sleep.
