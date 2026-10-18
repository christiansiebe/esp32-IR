#!/usr/bin/env python3
"""Send one message to a conversation and optionally run it.

Used to nudge a woken agent with its concrete open work, and to test the
send-message path. Stdlib only.
"""
import json
import os
import sys
import urllib.error
import urllib.request

BASE_URL = os.environ.get("OPENHANDS_BASE_URL", "https://app.all-hands.dev")


def main():
    if len(sys.argv) < 3:
        print("usage: nudge.py <conversation_id> <text> [--no-run]")
        return 2
    cid, text = sys.argv[1], sys.argv[2]
    run = "--no-run" not in sys.argv[3:]
    key = os.environ.get("OPENHANDS_API_KEY") or os.environ.get("OPENHANDS_CLOUD_API_KEY")
    if not key:
        print("OPENHANDS_API_KEY fehlt")
        return 2
    body = {"role": "user", "content": [{"type": "text", "text": text}], "run": run}
    req = urllib.request.Request(
        "%s/api/v1/app-conversations/%s/send-message" % (BASE_URL, cid),
        data=json.dumps(body).encode(), method="POST",
        headers={"Authorization": "Bearer " + key,
                 "Content-Type": "application/json",
                 "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            print(cid[:8], "HTTP", r.status, r.read().decode()[:200])
        return 0
    except urllib.error.HTTPError as e:
        print(cid[:8], "HTTP", e.code, e.read().decode()[:300])
        return 1


if __name__ == "__main__":
    sys.exit(main())
