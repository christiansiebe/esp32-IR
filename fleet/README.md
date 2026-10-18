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
        parent conversation  (this chat)
                 |  writes
                 v
        fleet/agents.json   <-- open_tasks per worker (0 = may sleep)
                 ^  reads
                 |
        fleet/fleet.py  (watchdog, kept alive by remote-shell/keepalive.sh)
                 |  polls every 300 s
                 v
   +-----------------------------------------------+
   | for each worker: GET app-conversations?ids=.. |
   |   sandbox PAUSED and open_tasks > 0 ?         |
   |     -> POST sandboxes/<id>/resume             |
   |     -> POST .../send-message  (wake nudge)    |
   +-----------------------------------------------+
```

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
