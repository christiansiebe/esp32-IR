#!/usr/bin/env python3
"""Deploy an automation from this repo to OpenHands Cloud.

The automation code does NOT run from the repository: each run unpacks a tarball
that was uploaded once. So editing automations/<name>/main.py here changes
nothing until the tarball is re-uploaded and the automation is updated. This
script does both.

  python3 fleet/deploy_automation.py fleet-supervisor
  python3 fleet/deploy_automation.py fleet-supervisor --id eb73ddf3-...

Reads OPENHANDS_API_KEY (or fleet/.api_key). Prints the automation JSON so the
schedule/state can be confirmed after the update.
"""
import argparse
import io
import json
import os
import sys
import tarfile
import urllib.error
import urllib.request

BASE_URL = os.environ.get("OPENHANDS_BASE_URL", "https://app.all-hands.dev").rstrip("/")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUTOMATIONS = os.path.join(REPO, "automations")


def api_key():
    key = os.environ.get("OPENHANDS_API_KEY") or os.environ.get("OPENHANDS_CLOUD_API_KEY")
    if key:
        return key
    path = os.path.join(REPO, "fleet", ".api_key")
    if os.path.exists(path):
        return open(path).read().strip()
    sys.exit("OPENHANDS_API_KEY fehlt (und keine fleet/.api_key)")


def api(method, path, body=None, raw=None, ctype="application/json", timeout=60):
    headers = {"Authorization": f"Bearer {api_key()}", "Accept": "application/json"}
    if raw is not None:
        headers["Content-Type"] = ctype
        data = raw
    else:
        data = json.dumps(body).encode() if body is not None else None
        if data is not None:
            headers["Content-Type"] = "application/json"
    req = urllib.request.Request(BASE_URL + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            txt = r.read().decode()
        return json.loads(txt) if txt.strip() else {}
    except urllib.error.HTTPError as e:
        sys.exit(f"{method} {path} -> HTTP {e.code}: {e.read().decode()[:300]}")


def build_tarball(name):
    """Pack automations/<name>/ as a gzip tarball whose root holds main.py."""
    src = os.path.join(AUTOMATIONS, name)
    if not os.path.isdir(src):
        sys.exit(f"nicht gefunden: {src}")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for fn in sorted(os.listdir(src)):
            if fn == "__pycache__":
                continue
            tar.add(os.path.join(src, fn), arcname=fn)
    return buf.getvalue()


def find_automation(name):
    data = api("GET", "/api/automation/v1?limit=50")
    for a in data.get("automations", []):
        if a.get("name", "").lower() == name.replace("-", " ").lower() or \
           name.split("-")[-1] in a.get("name", "").lower():
            return a
    return None


def main():
    ap = argparse.ArgumentParser(description="Deploy an automation tarball to OpenHands Cloud")
    ap.add_argument("name", help="automation directory name, e.g. fleet-supervisor")
    ap.add_argument("--id", help="automation id (skip name lookup)")
    ap.add_argument("--schedule", help="also set the cron schedule, e.g. '*/5 * * * *'")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    tarball = build_tarball(args.name)
    print(f"tarball: {len(tarball)} bytes aus automations/{args.name}/")

    if args.dry_run:
        print("dry-run: kein Upload")
        return

    up = api("POST", f"/api/automation/v1/uploads?name={args.name}", raw=tarball,
             ctype="application/gzip")
    tarball_path = up["tarball_path"]
    print(f"hochgeladen: {tarball_path}")

    aid = args.id
    if not aid:
        auto = find_automation(args.name)
        if not auto:
            sys.exit(f"Automation '{args.name}' nicht gefunden - --id angeben")
        aid = auto["id"]

    body = {"tarball_path": tarball_path}
    if args.schedule:
        body["trigger"] = {"type": "cron", "schedule": args.schedule, "timezone": "UTC"}
    out = api("PATCH", f"/api/automation/v1/{aid}", body=body)
    print(json.dumps({k: out.get(k) for k in
                      ("id", "name", "tarball_path", "trigger", "state", "enabled")},
                     indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
