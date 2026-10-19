#!/usr/bin/env python3
"""Parent-Seite: Job in die GitHub-Job-Queue fuer die Odroid-Worker stellen.

Die Worker auf dem Odroid POLLEN die Datei
`remote-shell/worker/jobs.json` (Branch, Default cline/power-history-diag) per
oeffentlichem raw-URL — read-only, kein Token auf dem Odroid noetig. Neue Jobs
werden automatisch aufgegriffen (kein cmd_send noetig).

  worker_enqueue.py "Analysiere X"
  worker_enqueue.py --id my-job-1 "Auftrag"
  worker_enqueue.py --list
  worker_enqueue.py --status <id>

Nur DIESE Parent-Seite braucht GITHUB_WRITE_TOKEN.
"""
import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

OWNER = os.environ.get("MEM_OWNER", "christiansiebe")
REPO = os.environ.get("MEM_REPO", "esp32-IR")
BRANCH = os.environ.get("WORKER_JOBS_BRANCH", "cline/power-history-diag")
PATH = os.environ.get("WORKER_JOBS_PATH", "remote-shell/worker/jobs.json")
API = "https://api.github.com"
RAW = f"https://raw.githubusercontent.com/{OWNER}/{REPO}/{BRANCH}/{PATH}"


def token():
    t = os.environ.get("GITHUB_WRITE_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not t:
        sys.exit("GITHUB_WRITE_TOKEN fehlt (nur die Parent-Seite braucht ihn).")
    return t


def load():
    req = urllib.request.Request(f"{API}/repos/{OWNER}/{REPO}/contents/{PATH}?ref={BRANCH}",
                                 headers={"Authorization": "Bearer " + token()})
    d = json.load(urllib.request.urlopen(req))
    return json.loads(base64.b64decode(d["content"])), d["sha"]


def save(data, sha, msg):
    body = {"message": msg,
            "content": base64.b64encode(json.dumps(data, indent=2).encode()).decode(),
            "branch": BRANCH, "sha": sha}
    req = urllib.request.Request(f"{API}/repos/{OWNER}/{REPO}/contents/{PATH}",
                                 data=json.dumps(body).encode(), method="PUT",
                                 headers={"Authorization": "Bearer " + token(),
                                          "Accept": "application/vnd.github+json"})
    return json.load(urllib.request.urlopen(req))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text", nargs="?")
    ap.add_argument("--id")
    ap.add_argument("--by", default="parent")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--status")
    a = ap.parse_args()

    if a.list or a.status:
        d, _ = load()
        jobs = d.get("jobs", {})
        if a.status:
            print(json.dumps(jobs.get(a.status, {}), indent=2, ensure_ascii=False))
        else:
            for jid, j in jobs.items():
                print(f"{jid:24} {j.get('state','open'):8} by={j.get('by','')} {j.get('text','')[:60]}")
        return

    if not a.text:
        sys.exit("Aufgabentext fehlt.")
    jid = a.id or f"{int(time.time())}-{os.getpid()}"
    d, sha = load()
    d.setdefault("jobs", {})[jid] = {
        "text": a.text, "by": a.by, "state": "open",
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    save(d, sha, f"worker: enqueue job {jid}")
    print(f"Job eingestellt: {jid}  (raw: {RAW})")
    print(f"Status: python3 worker_enqueue.py --status {jid}")


if __name__ == "__main__":
    main()
