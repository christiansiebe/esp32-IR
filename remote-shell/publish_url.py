#!/usr/bin/env python3
"""publish_url.py — aktuelle Tunnel-URL in die GitHub-Registry schreiben.

Der Odroid liest `remote-shell/memory/sandboxes.json` (Branch `channel-registry`)
alle 60 s und verbindet sich zu allen dort eingetragenen URLs. Damit ueberlebt
der Kanal Tunnel-Neustarts und serveo-Ablaufzeiten ohne Handarbeit.

Aufruf:  python3 publish_url.py https://<name>.serveousercontent.com
Auth:    gh CLI (bereits angemeldet) — kein Token im Code/Log.
"""
import base64
import json
import os
import subprocess
import sys
import time

OWNER = os.environ.get("MEM_OWNER", "christiansiebe")
REPO = os.environ.get("MEM_REPO", "esp32-IR")
BRANCH = os.environ.get("MEM_BRANCH", "channel-registry")
PATH = os.environ.get("MEM_REGISTRY", "remote-shell/memory/sandboxes.json")


def gh(*args, stdin=None):
    return subprocess.run(["gh", "api", *args], capture_output=True,
                          text=True, input=stdin)


def ensure_branch():
    if gh(f"repos/{OWNER}/{REPO}/git/ref/heads/{BRANCH}").returncode == 0:
        return True
    base = gh(f"repos/{OWNER}/{REPO}/git/ref/heads/main")
    if base.returncode != 0:
        return False
    sha = json.loads(base.stdout)["object"]["sha"]
    body = json.dumps({"ref": f"refs/heads/{BRANCH}", "sha": sha})
    return gh("-X", "POST", f"repos/{OWNER}/{REPO}/git/refs",
              "--input", "-", stdin=body).returncode == 0


def main():
    if len(sys.argv) < 2:
        print("Aufruf: publish_url.py <url>")
        return 2
    url = sys.argv[1].rstrip("/") + "/"
    if not ensure_branch():
        print("publish: Branch konnte nicht angelegt werden")
        return 1

    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    data = {"updated": now, "sandboxes": {"cline": {"url": url, "updated": now}}}
    content = base64.b64encode(json.dumps(data, indent=2).encode()).decode()

    sha = None
    cur = gh(f"repos/{OWNER}/{REPO}/contents/{PATH}?ref={BRANCH}")
    if cur.returncode == 0:
        sha = json.loads(cur.stdout).get("sha")

    body = {"message": f"channel registry: {url}", "content": content, "branch": BRANCH}
    if sha:
        body["sha"] = sha
    res = gh("-X", "PUT", f"repos/{OWNER}/{REPO}/contents/{PATH}",
             "--input", "-", stdin=json.dumps(body))
    if res.returncode != 0:
        print(f"publish: {res.stderr.strip()[:200]}")
        return 1
    print(f"publish: {url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
