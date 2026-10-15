#!/usr/bin/env python3
"""publish.py — alles unter EINEN neuen Ordner `remote-shell/` legen.

Der ESP-IDF-Firmware-Baum (main/, components/, tools/, CMakeLists.txt) bleibt
unberuehrt. Nur `.openhands/setup.sh` muss im Root bleiben (OpenHands-Hook) und
ist selbst-enthalten: es holt das Toolkit aus `remote-shell/` und startet es.

Neues Layout im Repo:
  remote-shell/<toolkit-dateien>
  remote-shell/memory/SHARED.md        (gemeinsames Gedaechtnis)
  remote-shell/memory/sandboxes.json   (Sandbox-Register fuer den Odroid)
  remote-shell/AGENTS.md               (Anleitung/Zeiger)
  .openhands/setup.sh                  (Auto-Hook, zwingend im Root)

Alte Pfade (tools/remote-shell/*, memory/*, AGENTS.md im Root) werden geloescht.

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
EXTRA_BRANCH = os.environ.get("MEM_EXTRA_BRANCH", "cline/power-history-diag")
TOKEN = os.environ.get("GITHUB_WRITE_TOKEN", "")
API = "https://api.github.com"

WS = "/workspace/remote-shell"
PROJ = "/workspace/project/9d40302f3e5e40f2a6ad48569ff4f140"
NEW = "remote-shell"

TOOLS = (
    "sandbox_term_server.py", "sandbox_term_client.py", "cmd_send.sh",
    "cmd_agent.py", "supervise.py", "register_sandbox.py", "sync-memory.sh",
    "bootstrap.sh", "OTHER-CHAT.md", "README.md", "install_autostart.sh",
    "publish.py", "pointer.py", "remote_exec.py", "AGENTS.md",
)

# repo_pfad -> lokale Datei
FILES = {f"{NEW}/{n}": f"{WS}/{n}" for n in TOOLS}
FILES[f"{NEW}/memory/SHARED.md"] = "/workspace/MEMORY.md"
FILES[".openhands/setup.sh"] = f"{PROJ}/.openhands/setup.sh"

# Alte Pfade, die aus dem Firmware-Baum verschwinden muessen.
OLD_PATHS = [f"tools/remote-shell/{n}" for n in TOOLS]
OLD_PATHS += ["AGENTS.md", "memory/SHARED.md", "memory/sandboxes.json"]


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


def put(repo_path, content_bytes, branch, msg):
    st, meta = api("GET", f"/repos/{OWNER}/{REPO}/contents/{repo_path}?ref={branch}")
    sha = meta.get("sha") if st == 200 else None
    body = {"message": msg, "content": base64.b64encode(content_bytes).decode(), "branch": branch}
    if sha:
        body["sha"] = sha
    st, resp = api("PUT", f"/repos/{OWNER}/{REPO}/contents/{repo_path}", body)
    return st in (200, 201), st, resp.get("message", "")


def rm(repo_path, branch):
    st, meta = api("GET", f"/repos/{OWNER}/{REPO}/contents/{repo_path}?ref={branch}")
    if st != 200 or not meta.get("sha"):
        return "n/a"
    st, _ = api("DELETE", f"/repos/{OWNER}/{REPO}/contents/{repo_path}",
                {"message": f"move: remove {repo_path}", "sha": meta["sha"], "branch": branch})
    return "geloescht" if st == 200 else f"FEHLER {st}"


def main():
    if not TOKEN:
        print("GITHUB_WRITE_TOKEN fehlt.")
        return 2
    mode = sys.argv[1] if len(sys.argv) > 1 else "publish"
    # Register-Inhalt einmalig vom alten Pfad uebernehmen.
    registry = None
    st, meta = api("GET", f"/repos/{OWNER}/{REPO}/contents/memory/sandboxes.json?ref={BRANCH}")
    if st == 200 and meta.get("content"):
        registry = base64.b64decode(meta["content"])
        print("Register-Inhalt uebernommen.")

    branches = [BRANCH] + ([EXTRA_BRANCH] if EXTRA_BRANCH and EXTRA_BRANCH != BRANCH else [])
    total = fails = 0
    for branch in branches:
        print(f"== Branch {branch} ==")
        if mode == "publish":
            for repo_path, local_path in FILES.items():
                if not os.path.exists(local_path):
                    print(f"  SKIP {repo_path} (fehlt lokal)"); continue
                ok, st, msg = put(repo_path, open(local_path, "rb").read(), branch,
                                  f"remote-shell: {repo_path}")
                print(f"  {'OK  ' if ok else 'FAIL'} {repo_path} [{branch}] {'' if ok else f'{st} {msg}'}")
                total += 1
                fails += 0 if ok else 1
            if registry is not None:
                ok, st, msg = put(f"{NEW}/memory/sandboxes.json", registry, branch,
                                  "remote-shell: sandboxes registry")
                print(f"  {'OK  ' if ok else 'FAIL'} {NEW}/memory/sandboxes.json [{branch}]")
                total += 1
                fails += 0 if ok else 1
        elif mode == "delete":
            for old in OLD_PATHS:
                print(f"  rm  {old} [{branch}] -> {rm(old, branch)}")
    print(f"fertig ({mode}): {total - fails}/{total} ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
