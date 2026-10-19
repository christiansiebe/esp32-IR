#!/usr/bin/env python3
"""Registriert diese Sandbox im GitHub-Register (memory/sandboxes.json).

Laeuft automatisch (via .openhands/setup.sh bzw. supervise.py). Der Odroid
liest das Register und verbindet sich zu ALLEN registrierten Sandboxes ->
kein manueller Eintrag auf dem Odroid noetig.

Env:
  RUNTIME_ID            (Pflicht) Sandbox-ID -> URL wird daraus gebaut
  SANDBOX_TERM_URL      optional: volle URL (Override)
  GITHUB_WRITE_TOKEN    (Pflicht zum Schreiben; als Cloud-Secret injiziert)
  MEM_OWNER/MEM_REPO/MEM_BRANCH/MEM_REGISTRY  optional (Defaults unten)
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

OWNER = os.environ.get("MEM_OWNER", "christiansiebe")
REPO = os.environ.get("MEM_REPO", "esp32-IR")
BRANCH = os.environ.get("MEM_BRANCH", "main")
REGISTRY = os.environ.get("MEM_REGISTRY", "remote-shell/memory/sandboxes.json")
SHARED = os.environ.get("MEM_SHARED", "remote-shell/memory/SHARED.md")
API = "https://api.github.com"


def url_for(runtime_id):
    return os.environ.get("SANDBOX_TERM_URL") or (
        f"wss://work-2-{runtime_id}.prod-runtime.all-hands.dev/")


def registry_url():
    return os.environ.get("SANDBOX_TERM_REGISTRY") or (
        f"https://raw.githubusercontent.com/{OWNER}/{REPO}/{BRANCH}/{REGISTRY}")


def _registry_payload(data):
    """Register-Inhalt fuer den Odroid: neue Pfade mitliefern."""
    data.setdefault("paths", {})["memory"] = SHARED
    data["paths"]["registry"] = registry_url()
    return data


# TTL: Eintraege aelter als N Stunden gelten als tot (Sandbox beendet) und
# werden beim naechsten Schreiben entfernt. Verhindert, dass sich das Register
# mit toten Sandboxes fuellt (der Odroid verbindet sich sonst endlos zu ihnen).
REGISTRY_TTL_HOURS = int(os.environ.get("SANDBOX_REGISTRY_TTL_HOURS", "12"))


def _parse_ts(ts):
    try:
        return time.mktime(time.strptime(ts, "%Y-%m-%dT%H:%M:%SZ"))
    except (ValueError, TypeError):
        return 0.0


def _prune_stale(data, keep_id):
    """Tote Eintraege (aelter als TTL) entfernen; keep_id nie entfernen."""
    now = time.time()
    ttl = REGISTRY_TTL_HOURS * 3600
    kept = {}
    for k, v in data.get("sandboxes", {}).items():
        if k == keep_id or (now - _parse_ts(v.get("updated", ""))) < ttl:
            kept[k] = v
    data["sandboxes"] = kept
    return data


def api(method, path, token, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Accept", "application/vnd.github+json")
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except Exception:
            return e.code, {}


def main():
    rid = os.environ.get("RUNTIME_ID", "").strip()
    token = os.environ.get("GITHUB_WRITE_TOKEN", "").strip()
    if not rid:
        print("register: RUNTIME_ID fehlt - uebersprungen.")
        return 0
    if not token:
        print("register: GITHUB_WRITE_TOKEN fehlt - uebersprungen.")
        return 0
    url = url_for(rid)

    path = f"/repos/{OWNER}/{REPO}/contents/{REGISTRY}"
    for attempt in range(4):
        status, meta = api("GET", f"{path}?ref={BRANCH}", token)
        sha = meta.get("sha")
        data = {"updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "sandboxes": {}}
        if status == 200 and meta.get("content"):
            try:
                import base64
                data = json.loads(base64.b64decode(meta["content"]).decode())
                data.setdefault("sandboxes", {})
            except Exception:
                pass
        data["sandboxes"][rid] = {"url": url, "updated": data["updated"]}
        _prune_stale(data, rid)
        _registry_payload(data)
        import base64
        body = {
            "message": f"memory: register sandbox {rid}",
            "content": base64.b64encode(json.dumps(data, indent=2).encode()).decode(),
            "branch": BRANCH,
        }
        if sha:
            body["sha"] = sha
        st, resp = api("PUT", path, token, body)
        if st in (200, 201):
            print(f"register: {rid} -> {url}  (HTTP {st})")
            return 0
        if st == 409:  # Konflikt -> neu lesen und erneut versuchen
            time.sleep(1)
            continue
        print(f"register: HTTP {st} {resp.get('message', '')}")
        return 1
    print("register: Konflikt blieb bestehen.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
