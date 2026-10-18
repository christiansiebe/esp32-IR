#!/usr/bin/env python3
"""Restart a chat agent that no longer responds properly.

Two levels, tried in order:

  1. If the agent is *running* but stuck, stop the wedged turn on its own
     runtime host (the org-level stop endpoint returns 404 on Cloud, and each
     conversation lives on its own host):
       POST {host}/api/conversations/{id}/interrupt
  2. Then reactivate it: resume its sandbox and send a wake message.
       POST /api/v1/sandboxes/{sandbox_id}/resume
       POST /api/v1/app-conversations/{id}/send-message

Stopping alone is not enough - a stopped conversation is idle, not running. The
resume + message step is what actually brings it back; interrupting first only
clears a stuck turn so the new message is not queued behind it.

Stdlib only. Usage:
    python3 restart.py <conversation_id> [message] [--no-stop]
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request
from urllib.parse import urlsplit

BASE_URL = os.environ.get("OPENHANDS_BASE_URL", "https://app.all-hands.dev")
DEFAULT_MSG = (
    "System: Dein vorheriger Lauf wurde zurueckgesetzt (Stop + Resume). Bitte "
    "mache mit deiner offenen Aufgabe weiter; arbeite ueber Sub-Agenten und "
    "beende erst, wenn keine offene Aufgabe mehr existiert."
)


def key():
    k = os.environ.get("OPENHANDS_API_KEY") or os.environ.get("OPENHANDS_CLOUD_API_KEY")
    if k:
        return k
    try:
        return open("/workspace/fleet/.api_key").read().strip()
    except OSError:
        return None


def _req(url, headers, body, method, timeout):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read().decode()
    return json.loads(raw) if raw.strip() else {}


def api(method, path, body=None, timeout=40, base=None):
    return _req((base or BASE_URL) + path,
                {"Authorization": "Bearer " + key(), "Accept": "application/json",
                 "Content-Type": "application/json"},
                body, method, timeout)


def get_conv(cid, tries=4):
    for _ in range(tries):
        try:
            rec = api("GET", "/api/v1/app-conversations?ids=" + cid)
            items = rec.get("items") if isinstance(rec, dict) else rec
            if items:
                return items[0]
        except Exception:  # noqa: BLE001 - flaky endpoint, retry
            pass
        time.sleep(2)
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cid = sys.argv[1]
    msg = next((a for a in sys.argv[2:] if not a.startswith("--")), DEFAULT_MSG)
    do_stop = "--no-stop" not in sys.argv

    conv = get_conv(cid)
    if not conv:
        print("Konversation nicht gefunden (oder API flaky).")
        return 1
    sandbox = conv.get("sandbox_id")
    exec_status = conv.get("execution_status")
    print("vorher: sandbox=%s exec=%s" % (conv.get("sandbox_status"), exec_status))

    host, sk = None, conv.get("session_api_key")
    if conv.get("conversation_url"):
        sp = urlsplit(conv["conversation_url"])
        host = "%s://%s" % (sp.scheme, sp.netloc)

    if do_stop and exec_status == "running" and host:
        try:
            _req("%s/api/conversations/%s/interrupt" % (host, cid),
                 {"X-Session-API-Key": sk, "Content-Type": "application/json",
                  "Accept": "application/json"}, {}, "POST", 30)
            print("interrupt: ok")
            time.sleep(2)
        except urllib.error.HTTPError as e:
            print("interrupt: HTTP %s (weiter mit resume)" % e.code)
        except Exception as e:  # noqa: BLE001
            print("interrupt fehlgeschlagen: %s (weiter mit resume)" % e)

    if sandbox:
        try:
            api("POST", "/api/v1/sandboxes/%s/resume" % sandbox, {})
            print("resume: ok")
        except Exception as e:  # noqa: BLE001
            print("resume: %s" % e)

    try:
        api("POST", "/api/v1/app-conversations/%s/send-message" % cid,
            {"role": "user", "content": [{"type": "text", "text": msg}], "run": True})
        print("Weck-Nachricht: ok")
    except Exception as e:  # noqa: BLE001
        print("Weck-Nachricht: %s" % e)

    time.sleep(4)
    after = get_conv(cid)
    if after:
        print("nachher: sandbox=%s exec=%s" % (after.get("sandbox_status"),
                                               after.get("execution_status")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
