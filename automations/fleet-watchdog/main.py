"""Fleet watchdog: keep esp32-IR chat agents awake while they have open work.

Deterministic, no LLM, no token cost. Runs on the platform scheduler, so it
lives outside every conversation sandbox and therefore survives a sandbox pause
- which is exactly why it, and not an in-sandbox loop, is what makes the
"agents keep each other awake" rule actually hold.

Each cycle:
  1. read the worker registry (fleet/agents.json) - open_tasks per conversation
  2. for every worker with open_tasks > 0 whose sandbox is PAUSED:
       - resume the sandbox
       - send a short wake message pointing at the open work
  3. workers with open_tasks == 0 are left asleep

Safety rails: skip sandboxes idle longer than MAX_AGE_HOURS (surely finished).
"""
import json
import os
import sys
import urllib.request

BASE_URL = os.environ.get("OPENHANDS_CLOUD_API_URL", "https://app.all-hands.dev").rstrip("/")

# Public repo, so the registry can be fetched without a token.
REGISTRY_URLS = [
    "https://raw.githubusercontent.com/christiansiebe/esp32-IR/main/fleet/agents.json",
    "https://raw.githubusercontent.com/christiansiebe/esp32-IR/feat/dfs-clock-visibility/fleet/agents.json",
]
FALLBACK_REGISTRY = {
    "agents": {
        "a3484f59487c4e6a9f1c5a40664506ba": {"name": "UTC-persist", "open_tasks": 0},
        "335a48a5e35442e69b8f1cb1c9321480": {"name": "TWDT", "open_tasks": 1},
        "1b043863d80d41a08f7b161036f65649": {"name": "MQTT-reboot", "open_tasks": 1},
        "8c2548251ad045599fbfb6f95f3e7944": {"name": "parent", "open_tasks": 1},
    }
}

MAX_AGE_HOURS = float(os.environ.get("FLEET_MAX_AGE_HOURS", "72"))
WAKE_MESSAGE = (
    "System: Deine Sandbox war pausiert und wurde vom Fleet-Watchdog geweckt. "
    "Arbeite an deinen offenen Aufgaben weiter. Regel: arbeite ueber Sub-Agenten "
    "und beende erst, wenn keine offene Aufgabe mehr existiert. Wenn alles fertig "
    "ist, setze deine TASKS.json auf done und aktualisiere /workspace/fleet/agents.json."
)


def get_secret(name):
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
                url,
                data=json.dumps(body).encode(),
                headers={
                    "Content-Type": "application/json",
                    "Authorization": f"Bearer {os.environ.get('AUTOMATION_CALLBACK_API_KEY', '')}",
                },
            ),
            timeout=30,
        )
    except Exception as exc:  # noqa: BLE001 - callback must never mask the result
        print(f"Callback error: {exc}")


def api(token, path, method="GET", body=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Authorization": f"Bearer {token}", "Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"{BASE_URL}{path}", data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode()
        return resp.status, (json.loads(raw) if raw.strip() else None)


def load_registry():
    for url in REGISTRY_URLS:
        try:
            with urllib.request.urlopen(url, timeout=20) as r:
                reg = json.loads(r.read().decode())
            agents = reg.get("agents", {})
            if agents:
                print(f"registry: {len(agents)} agents von {url}")
                return agents
        except Exception as exc:  # noqa: BLE001 - try the next URL
            print(f"registry {url} nicht lesbar: {exc}")
    print("registry: Fallback-Liste")
    return FALLBACK_REGISTRY["agents"]


def hours_since(iso):
    if not iso:
        return None
    try:
        from datetime import datetime, timezone
        ts = datetime.fromisoformat(iso.replace("Z", "+00:00"))
        return (datetime.now(timezone.utc) - ts).total_seconds() / 3600.0
    except ValueError:
        return None


def main():
    token = get_secret("OPENHANDS_API_KEY")
    registry = load_registry()
    woken = []
    for cid, entry in registry.items():
        name = (entry or {}).get("name", cid[:8])
        open_n = (entry or {}).get("open_tasks", 0)
        try:
            _, rec = api(token, f"/api/v1/app-conversations?ids={cid}")
            items = rec.get("items") if isinstance(rec, dict) else rec
            if not items:
                print(f"{name}: nicht gefunden")
                continue
            it = items[0]
            status = it.get("sandbox_status")
            exec_status = it.get("execution_status")
            sandbox_id = it.get("sandbox_id")
            idle_h = hours_since(it.get("updated_at"))
            if open_n <= 0:
                print(f"{name}: 0 offene Aufgaben - schlafen lassen")
                continue
            if idle_h is not None and idle_h > MAX_AGE_HOURS:
                print(f"{name}: {idle_h:.0f}h inaktiv (> {MAX_AGE_HOURS:.0f}h) - schlafen lassen")
                continue
            # A conversation that inherits its parent's sandbox (parent_conversation_id)
            # shows execution_status=paused while sandbox_status stays RUNNING, because
            # the shared sandbox is kept alive by the parent. Keying only on
            # sandbox_status therefore never wakes such a worker. Treat either signal
            # as "asleep".
            asleep = status == "PAUSED" or exec_status == "paused"
            if not asleep:
                print(f"{name}: {status}/{exec_status}, offene Aufgaben={open_n} - ok")
                continue
            if not sandbox_id:
                print(f"{name}: schlaeft ohne sandbox_id - uebersprungen")
                continue
            if status == "PAUSED":
                api(token, f"/api/v1/sandboxes/{sandbox_id}/resume", method="POST", body={})
            try:
                api(token, f"/api/v1/app-conversations/{cid}/send-message", method="POST",
                    body={"role": "user", "content": [{"type": "text", "text": WAKE_MESSAGE}],
                          "run": True})
            except Exception as exc:  # noqa: BLE001 - resume already done
                print(f"{name}: Weck-Nachricht fehlgeschlagen: {exc}")
            print(f"{name}: geweckt (offene Aufgaben={open_n})")
            woken.append(name)
        except Exception as exc:  # noqa: BLE001 - one bad agent must not stop the rest
            print(f"{name}: Fehler: {exc}")
    print(f"fertig: {len(woken)} geweckt {woken}")
    fire_callback("COMPLETED")


try:
    main()
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: {exc}", file=sys.stderr)
    fire_callback("FAILED", str(exc))
    sys.exit(1)
