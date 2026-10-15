#!/usr/bin/env python3
"""publish.py — Toolkit + Setup-Hook + Memory ins GitHub-Repo (Branch main) legen.

Danach greift `.openhands/setup.sh` in JEDER Sandbox automatisch: Toolkit holen,
Token aus dem Cloud-Secret setzen, Supervisor starten, Sandbox registrieren.

Aufruf:  GITHUB_WRITE_TOKEN=... python3 publish.py
"""
import base64
import json
import os
import sys
import urllib.error
import urllib.request

OWNER = os.environ.get("MEM_OWNER", "christiansiebe")
REPO = os.environ.get("MEM_REPO", "esp32-IR")
BRANCH = os.environ.get("MEM_BRANCH", "main")
TOKEN = os.environ.get("GITHUB_WRITE_TOKEN", "")
API = "https://api.github.com"

WS = "/workspace/remote-shell"
PROJ = "/workspace/project/9d40302f3e5e40f2a6ad48569ff4f140"

# repo_pfad -> lokale Datei
FILES = {
    f"tools/remote-shell/{n}": f"{WS}/{n}"
    for n in (
        "sandbox_term_server.py", "sandbox_term_client.py", "cmd_send.sh",
        "cmd_agent.py", "supervise.py", "register_sandbox.py", "sync-memory.sh",
        "bootstrap.sh", "OTHER-CHAT.md", "README.md", "install_autostart.sh",
        "publish.py", "pointer.py", "remote_exec.py",
    )
}
FILES[".openhands/setup.sh"] = f"{PROJ}/.openhands/setup.sh"
FILES["AGENTS.md"] = f"{WS}/AGENTS.md"
FILES["memory/SHARED.md"] = "/workspace/MEMORY.md"


def api(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method)
    req.add_header("Authorization", f"Bearer {TOKEN}")
    req.add_header("Accept", "application/vnd.github+json")
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except Exception:
            return e.code, {}


def push_one(repo_path, local_path):
    if not os.path.exists(local_path):
        return f"SKIP (fehlt lokal): {repo_path}"
    st, meta = api("GET", f"/repos/{OWNER}/{REPO}/contents/{repo_path}?ref={BRANCH}")
    sha = meta.get("sha") if st == 200 else None
    content = base64.b64encode(open(local_path, "rb").read()).decode()
    body = {"message": f"toolkit: publish {repo_path}", "content": content, "branch": BRANCH}
    if sha:
        body["sha"] = sha
    st, resp = api("PUT", f"/repos/{OWNER}/{REPO}/contents/{repo_path}", body)
    if st in (200, 201):
        return f"OK   {repo_path}"
    return f"FAIL {repo_path}: {st} {resp.get('message', '')}"


def main():
    if not TOKEN:
        print("GITHUB_WRITE_TOKEN fehlt.")
        return 2
    fails = 0
    for repo_path, local_path in FILES.items():
        msg = push_one(repo_path, local_path)
        print(" ", msg)
        if msg.startswith("FAIL"):
            fails += 1
    print(f"fertig: {len(FILES) - fails}/{len(FILES)} ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
