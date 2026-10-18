"""Fleet supervisor: keep the right agents awake, let the rest sleep.

Deterministic, no LLM, no token cost. Runs on the platform scheduler, outside
every sandbox, so it survives a sandbox pause.

Each cycle, for every worker in the registry:
  * open_tasks > 0 and sandbox PAUSED, or the
    agent loop is not running (execution_status paused/finished/idle)
                                               -> resume + wake message
  * open_tasks > 0 and running, but no event
    for longer than STUCK_MINUTES              -> interrupt (clear a wedged turn)
                                                  + wake message
  * open_tasks == 0 and sandbox RUNNING, not
    shared with a worker that still has work   -> pause (stop paying for idle)
  * open_tasks == 0 and PAUSED                 -> leave asleep

Stuck detection uses the conversation's last activity time, not the platform's
updated_at, which does not move while an agent sits idle.

Two platform behaviours this has to respect:
  * Several workers can share one sandbox (a worker started with
    parent_conversation_id inherits the parent's). Such a worker keeps
    sandbox_status=RUNNING while execution_status drops to paused after a
    turn - keying on sandbox_status alone never wakes it. Pausing that
    sandbox because one tenant is done would take the others down.
  * send-message answers HTTP 409 while a sandbox is still STARTING, so a
    resume followed immediately by send-message silently loses the wake. The
    sandbox is polled until RUNNING first.

State between runs (consecutive wake attempts per conversation) lives in the KV
store, so a genuinely broken agent is not retried forever.
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from urllib.parse import urlsplit

BASE_URL = os.environ.get("OPENHANDS_CLOUD_API_URL", "https://app.all-hands.dev").rstrip("/")

REGISTRY_URLS = [
    "https://raw.githubusercontent.com/christiansiebe/esp32-IR/main/fleet/agents.json",
    "https://raw.githubusercontent.com/christiansiebe/esp32-IR/feat/dfs-clock-visibility/fleet/agents.json",
]
FALLBACK_REGISTRY = {
    "agents": {
        "a3484f59487c4e6a9f1c5a40664506ba": {"name": "UTC-persist", "open_tasks": 1},
        "335a48a5e35442e69b8f1cb1c9321480": {"name": "TWDT", "open_tasks": 1},
        "1b043863d80d41a08f7b161036f65649": {"name": "MQTT-reboot", "open_tasks": 1},
        "8c2548251ad045599fbfb6f95f3e7944": {"name": "parent", "open_tasks": 1},
    }
}

MAX_AGE_HOURS = float(os.environ.get("FLEET_MAX_AGE_HOURS", "72"))
STUCK_MINUTES = float(os.environ.get("FLEET_STUCK_MINUTES", "20"))
MAX_WAKES = int(os.environ.get("FLEET_MAX_WAKES", "6"))
RESUME_WAIT_SECONDS = float(os.environ.get("FLEET_RESUME_WAIT_SECONDS", "90"))
# The parent hosts the fleet tooling; pausing it would stop nothing critical
# (the cron is the guarantee) but it is the coordinator, so leave it running.
NEVER_PAUSE = {"8c2548251ad045599fbfb6f95f3e7944"}

WAKE_MESSAGE = (
    "System: Deine Sandbox war pausiert bzw. dein Lauf festgefahren und wurde vom "
    "Fleet-Supervisor geweckt. Arbeite an deinen offenen Aufgaben weiter. Regel: "
    "arbeite ueber Sub-Agenten und beende erst, wenn keine offene Aufgabe mehr "
    "existiert. Wenn alles fertig ist, setze deine TASKS.json auf done und "
    "aktualisiere /workspace/fleet/agents.json."
)

# --- KV store (state between runs) -----------------------------------------
_KV_TOKEN = os.environ.get("AUTOMATION_KV_TOKEN", "")
_KV_BASE = os.environ.get("AUTOMATION_API_URL", "").rstrip("/")
_STATE_KEY = "state"


def kv_available():
    return bool(_KV_TOKEN and _KV_BASE)


def kv_get(key):
    req = urllib.request.Request(f"{_KV_BASE}/v1/kv/{key}",
                                 headers={"Authorization": f"Bearer {_KV_TOKEN}"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read())["value"]
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            return None
        raise


def kv_set(key, value):
    req = urllib.request.Request(
        f"{_KV_BASE}/v1/kv/{key}", data=json.dumps(value).encode(), method="PUT",
        headers={"Authorization": f"Bearer {_KV_TOKEN}", "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        r.read()


def load_state():
    if kv_available():
        return kv_get(_STATE_KEY) or {"version": 1, "conv": {}}
    return {"version": 1, "conv": {}}


def save_state(state):
    if kv_available():
        kv_set(_STATE_KEY, state)


# --- helpers ----------------------------------------------------------------

def get_secret(name):
    # Local/dev override: the key may already be in the environment.
    if os.environ.get(name):
        return os.environ[name]
    url = os.environ.get("AGENT_SERVER_URL", "").rstrip("/")
    key = os.environ.get("SESSION_API_KEY") or os.environ.get("OH_SESSION_API_KEYS_0", "")
    if url:  # local Agent Canvas
        url = f"{url}/api/settings/secrets/{name}"
    else:  # OpenHands Cloud / Enterprise
        api = os.environ["OPENHANDS_CLOUD_API_URL"].rstrip("/")
        url = f"{api}/api/v1/sandboxes/{os.environ['SANDBOX_ID']}/settings/secrets/{name}"
    with urllib.request.urlopen(
        urllib.request.Request(url, headers={"X-Session-API-Key": key}), timeout=30
    ) as r:
        return r.read().decode().strip()


def fire_callback(status="COMPLETED", error=None):
    url = os.environ.get("AUTOMATION_CALLBACK_URL", "")
    if not url:
        return
    body = {"status": status, "run_id": os.environ.get("AUTOMATION_RUN_ID", "")}
    if error:
        body["error"] = error
    try:
        urllib.request.urlopen(
            urllib.request.Request(
                url, data=json.dumps(body).encode(),
                headers={"Content-Type": "application/json",
                         "Authorization": f"Bearer {os.environ.get('AUTOMATION_CALLBACK_API_KEY', '')}"},
            ), timeout=30)
    except Exception as exc:  # noqa: BLE001
        print(f"Callback error: {exc}")


def _req(url, headers, body, method, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read().decode()
    return json.loads(raw) if raw.strip() else {}


def cloud(token, method, path, body=None, timeout=30):
    return _req(BASE_URL + path,
                {"Authorization": f"Bearer {token}", "Accept": "application/json",
                 "Content-Type": "application/json"}, body, method, timeout)


def load_registry():
    for url in REGISTRY_URLS:
        try:
            with urllib.request.urlopen(url, timeout=20) as r:
                agents = json.loads(r.read().decode()).get("agents", {})
            if agents:
                print(f"registry: {len(agents)} agents von {url}")
                return agents
        except Exception as exc:  # noqa: BLE001
            print(f"registry {url} nicht lesbar: {exc}")
    print("registry: Fallback-Liste")
    return FALLBACK_REGISTRY["agents"]


def hours_since(iso):
    if not iso:
        return None
    try:
        ts = datetime.fromisoformat(iso.replace("Z", "+00:00"))
        return (datetime.now(timezone.utc) - ts).total_seconds() / 3600.0
    except ValueError:
        return None


def minutes_since(iso):
    h = hours_since(iso)
    return None if h is None else h * 60.0


def interrupt(conv):
    host = conv.get("conversation_url")
    if not host:
        return False
    sp = urlsplit(host)
    try:
        _req("%s://%s/api/conversations/%s/interrupt" % (sp.scheme, sp.netloc, conv["id"]),
             {"X-Session-API-Key": conv.get("session_api_key", ""),
              "Content-Type": "application/json", "Accept": "application/json"},
             {}, "POST", 20)
        return True
    except Exception:  # noqa: BLE001
        return False


def sandbox_status(token, sandbox_id):
    """Authoritative sandbox status. The app-conversation record can be stale
    (it may say RUNNING while the sandbox is really PAUSED)."""
    try:
        rec = cloud(token, "GET", f"/api/v1/sandboxes?id={sandbox_id}")
        items = rec if isinstance(rec, list) else rec.get("items", [])
        for it in items:
            if it.get("id") == sandbox_id:
                return it.get("status")
    except Exception:  # noqa: BLE001
        pass
    return None


def wake_worker(token, cid, conv):
    """Resume the sandbox if it is not RUNNING, wait until it is, then send the
    wake message. send-message returns 409 while the sandbox is STARTING, so
    resuming and sending in one breath silently drops the wake. The authoritative
    status is queried because a shared sandbox's per-conversation record can be
    stale."""
    sandbox_id = conv.get("sandbox_id")
    if sandbox_id:
        if sandbox_status(token, sandbox_id) not in ("RUNNING", None):
            try:
                cloud(token, "POST", f"/api/v1/sandboxes/{sandbox_id}/resume", {})
            except Exception:  # noqa: BLE001
                pass
        deadline = time.monotonic() + RESUME_WAIT_SECONDS
        while time.monotonic() < deadline:
            if sandbox_status(token, sandbox_id) in (None, "RUNNING"):
                break
            time.sleep(5)
    cloud(token, "POST", f"/api/v1/app-conversations/{cid}/send-message",
          {"role": "user", "content": [{"type": "text", "text": WAKE_MESSAGE}],
           "run": True})


def main():
    token = get_secret("OPENHANDS_API_KEY")
    registry = load_registry()
    state = load_state()
    conv_state = state.setdefault("conv", {})
    woken, paused, stuck = [], [], []

    # First pass: fetch every conversation and note, per sandbox, the highest
    # open_tasks among its tenants. A sandbox is shared by several workers, so
    # pausing it because ONE tenant is done would take the others down.
    records = {}
    sandbox_open = {}
    for cid, entry in registry.items():
        name = (entry or {}).get("name", cid[:8])
        open_n = (entry or {}).get("open_tasks", 0)
        try:
            rec = cloud(token, "GET", f"/api/v1/app-conversations?ids={cid}")
            items = rec.get("items") if isinstance(rec, dict) else rec
            if not items:
                print(f"{name}: nicht gefunden")
                continue
            conv = items[0]
            records[cid] = (conv, open_n, name)
            sid = conv.get("sandbox_id")
            if sid:
                sandbox_open[sid] = max(sandbox_open.get(sid, 0), open_n)
        except Exception as exc:  # noqa: BLE001 - one bad agent must not stop the rest
            print(f"{name}: Fehler: {exc}")

    for cid, (conv, open_n, name) in records.items():
        status = conv.get("sandbox_status")
        exec_status = conv.get("execution_status")
        sandbox_id = conv.get("sandbox_id")
        idle_h = hours_since(conv.get("updated_at"))
        rec_state = conv_state.setdefault(cid, {})
        # The agent loop is not running: either the sandbox is down or, for a
        # worker sharing a parent sandbox, execution_status went idle while
        # sandbox_status stayed RUNNING.
        asleep = status == "PAUSED" or exec_status in ("paused", "finished", "idle")

        if open_n <= 0:
            shared_with_work = bool(sandbox_id) and sandbox_open.get(sandbox_id, 0) > 0
            if (status == "RUNNING" and cid not in NEVER_PAUSE and sandbox_id
                    and not shared_with_work):
                try:
                    cloud(token, "POST", f"/api/v1/sandboxes/{sandbox_id}/pause", {})
                    print(f"{name}: fertig -> pausiert (Kosten sparen)")
                    paused.append(name)
                except Exception as e:  # noqa: BLE001
                    print(f"{name}: pause fehlgeschlagen: {e}")
            else:
                print(f"{name}: 0 offene Aufgaben - schlafen lassen")
            rec_state["wakes"] = 0
            continue

        if idle_h is not None and idle_h > MAX_AGE_HOURS:
            print(f"{name}: {idle_h:.0f}h inaktiv (> {MAX_AGE_HOURS:.0f}h) - schlafen lassen")
            continue

        if rec_state.get("wakes", 0) >= MAX_WAKES:
            print(f"{name}: {rec_state['wakes']} Weckversuche ohne Erfolg - uebersprungen (defekt?)")
            continue

        secs = minutes_since(conv.get("updated_at"))
        is_stuck = (status == "RUNNING" and not asleep and secs is not None
                    and secs > STUCK_MINUTES)

        if asleep or is_stuck:
            if is_stuck and interrupt(conv):
                print(f"{name}: festgefahren ({secs:.0f} min ohne Aktivitaet) -> interrupt")
                stuck.append(name)
            try:
                wake_worker(token, cid, conv)
                rec_state["wakes"] = rec_state.get("wakes", 0) + 1
                rec_state["last_wake"] = datetime.now(timezone.utc).isoformat()
                woken.append(name)
                print(f"{name}: geweckt (offene Aufgaben={open_n}, Versuch {rec_state['wakes']})")
            except Exception as e:  # noqa: BLE001
                print(f"{name}: Weck-Nachricht fehlgeschlagen: {e}")
        else:
            print(f"{name}: {status}/{exec_status}, offene Aufgaben={open_n} - ok")

    save_state(state)
    print(f"fertig: geweckt={woken} pausiert={paused} festgefahren={stuck}")
    fire_callback("COMPLETED")


try:
    main()
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: {exc}", file=sys.stderr)
    fire_callback("FAILED", str(exc))
    sys.exit(1)
