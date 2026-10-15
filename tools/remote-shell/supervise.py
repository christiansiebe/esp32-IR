#!/usr/bin/env python3
"""Supervisor fuer den Sandbox-Terminal-/Kommando-Server.

Startet ``sandbox_term_server.py`` im Hintergrund, haelt ihn am Leben
(Neustart bei Absturz) und liest das Token aus einer Datei, damit eine
Token-Rotation ohne Neustart moeglich ist.

Aufruf:
    python3 tools/remote-shell/supervise.py            # starten (idempotent)
    python3 tools/remote-shell/supervise.py --status
    python3 tools/remote-shell/supervise.py --stop

Token/URL:
  - Token: $SANDBOX_TERM_TOKEN_FILE (Default: <projekt>/.remote-shell/token),
    Fallback $SANDBOX_TERM_TOKEN. Fehlt beides, wird ein Token erzeugt.
  - Oeffentliche URL: aus $RUNTIME_ID abgeleitet
    -> wss://work-2-<id>.prod-runtime.all-hands.dev/
  - Laufzeitdateien: <projekt>/.remote-shell/{supervisor.pid,server.pid,server.log}
    (ausserhalb des Repos, damit nichts committet wird).
"""
import argparse
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.abspath(os.path.join(HERE, "..", ".."))
# State liegt NEBEN dem Repo (nie im Git-Baum): Token + PIDs + Log.
STATE = os.environ.get("SANDBOX_TERM_STATE", os.path.join(os.path.dirname(PROJECT), ".remote-shell"))
TOKEN_FILE = os.environ.get("SANDBOX_TERM_TOKEN_FILE", os.path.join(STATE, "token"))
PID_FILE = os.path.join(STATE, "supervisor.pid")
SERVER_PID = os.path.join(STATE, "server.pid")
LOG_FILE = os.path.join(STATE, "server.log")
SERVER = os.path.join(HERE, "sandbox_term_server.py")
PORT = os.environ.get("SANDBOX_TERM_PORT", "12001")


def ensure_token():
    os.makedirs(STATE, exist_ok=True)
    if not os.path.exists(TOKEN_FILE) or not open(TOKEN_FILE).read().strip():
        import secrets
        with open(TOKEN_FILE, "w") as fh:
            fh.write(secrets.token_urlsafe(32))
        os.chmod(TOKEN_FILE, 0o600)
    return open(TOKEN_FILE).read().strip()


def public_url():
    rid = os.environ.get("RUNTIME_ID", "")
    if not rid:
        return "(RUNTIME_ID unbekannt)"
    return f"wss://work-2-{rid}.prod-runtime.all-hands.dev/"


def alive(pid):
    try:
        with open(f"/proc/{pid}/stat") as fh:
            state = fh.read().rsplit(")", 1)[1].split()[0]
        return state != "Z"
    except OSError:
        return False


def read_pid(path):
    try:
        return int(open(path).read().strip())
    except (OSError, ValueError):
        return None


def stop():
    for path in (PID_FILE, SERVER_PID):
        pid = read_pid(path)
        if pid and alive(pid):
            try:
                os.kill(pid, signal.SIGTERM)
                print(f"gestoppt: {path} (pid {pid})")
            except OSError:
                pass
        try:
            os.remove(path)
        except OSError:
            pass


def status():
    for name, path in (("supervisor", PID_FILE), ("server", SERVER_PID)):
        pid = read_pid(path)
        state = "laeuft" if pid and alive(pid) else "gestoppt"
        print(f"{name}: {state}" + (f" (pid {pid})" if pid else ""))
    print(f"token file: {TOKEN_FILE}")
    print(f"url:        {public_url()}")


def run_server():
    token = open(TOKEN_FILE).read().strip()
    env = dict(os.environ)
    env["SANDBOX_TERM_TOKEN"] = token
    env["SANDBOX_TERM_PORT"] = PORT
    with open(LOG_FILE, "a") as log:
        return subprocess.Popen(
            [sys.executable, SERVER], env=env, stdout=log, stderr=log,
            start_new_session=True,
        )


def supervise():
    while True:
        proc = run_server()
        with open(SERVER_PID, "w") as fh:
            fh.write(str(proc.pid))
        proc.wait()
        time.sleep(2)


def start():
    pid = read_pid(PID_FILE)
    if pid and alive(pid):
        print(f"supervisor laeuft bereits (pid {pid})")
        return
    token = ensure_token()
    os.makedirs(STATE, exist_ok=True)
    with open(PID_FILE, "w") as fh:
        fh.write(str(os.getpid()))
    print(f"supervisor gestartet (pid {os.getpid()}), Token {len(token)} Zeichen")
    print(f"url: {public_url()}")
    supervise()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--stop", action="store_true")
    args = ap.parse_args()
    if args.stop:
        stop()
    elif args.status:
        status()
    else:
        start()


if __name__ == "__main__":
    main()
