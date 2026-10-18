#!/usr/bin/env python3
"""Agent side of the reverse command channel (runs in the sandbox).

  python3 /workspace/remote-shell/channel.py send "<shell command>"
  python3 /workspace/remote-shell/channel.py wait [timeout_s]
  python3 /workspace/remote-shell/channel.py results

`send` queues a command and prints its id. `wait` blocks until that command's
output arrives (the device side polls and posts it back). `results` dumps every
stored output. Kept dependency-free (urllib only).
"""
import os
import subprocess
import sys
import time
import urllib.request

BASE = "/workspace/remote-shell"
QUEUE = os.path.join(BASE, "queue")
RESULTS = os.path.join(BASE, "results")
KEEPALIVE = os.path.join(BASE, "keepalive.sh")
LOCKFILE = os.path.join(BASE, "keepalive.lock")
HEALTH_URL = "http://127.0.0.1:12001/health"


def key():
    try:
        return open(os.path.join(BASE, "KEY")).read().strip()
    except FileNotFoundError:
        print("(noch kein Key registriert - Geraet muss sich einmalig anmelden)")
        return ""


def health_ok(timeout=3):
    try:
        req = urllib.request.Request(HEALTH_URL, headers={"X-Channel-Key": key()})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status == 200
    except Exception:
        return False


def keepalive_running():
    """True while a keepalive holds the flock on keepalive.lock."""
    import fcntl
    try:
        fh = open(LOCKFILE, "w")
    except OSError:
        return False
    try:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fh, fcntl.LOCK_UN)
        return False
    except OSError:
        return True
    finally:
        fh.close()


def ensure_helpers(wait_s=25):
    """Bring the sandbox-side helpers back if they died.

    A sandbox restart kills every background process (PID 1 is openhands-agent,
    there is no systemd/cron), so the channel server and the upload server are
    gone until something restarts them. Running this before every send means the
    channel is never used while its server is dead - no manual restart needed.
    keepalive.sh guards itself with an atomic lock, so starting it when one is
    already running is harmless; we still check first to avoid a needless spawn.
    """
    if health_ok():
        return True
    if keepalive_running():
        print("(channel server down - warte auf keepalive)", file=sys.stderr)
    elif os.path.exists(KEEPALIVE):
        print("(channel server down - starte keepalive.sh)", file=sys.stderr)
        with open(os.path.join(BASE, "keepalive.log"), "a") as log:
            subprocess.Popen(["setsid", "nohup", "bash", KEEPALIVE],
                             stdout=log, stderr=subprocess.STDOUT,
                             start_new_session=True)
    for _ in range(wait_s):
        time.sleep(1)
        if health_ok():
            print("(channel server wieder oben)", file=sys.stderr)
            return True
    print("(WARNUNG: channel server kam nicht hoch)", file=sys.stderr)
    return False


def _get(path):
    req = urllib.request.Request("http://127.0.0.1:12001" + path,
                                 headers={"X-Channel-Key": key()})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.read().decode("utf-8", "replace")
    except Exception as e:  # 204/no-body and errors both land here
        return getattr(e, "read", lambda: b"")().decode("utf-8", "replace") if hasattr(e, "read") else ""


def send(cmd):
    ensure_helpers()
    cid = "%d-%d" % (int(time.time()), os.getpid())
    with open(os.path.join(QUEUE, cid + ".json"), "w") as fh:
        fh.write(cmd)
    print(cid)
    return cid


def results():
    print(_get("/results"))


def wait(cid, timeout):
    out = os.path.join(RESULTS, cid + ".out")
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(out):
            print(open(out, encoding="utf-8", errors="replace").read())
            return 0
        time.sleep(2)
    print("(timeout nach %ds - noch keine Ausgabe fuer %s)" % (timeout, cid))
    return 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    cmd = sys.argv[1]
    if cmd == "send":
        send(sys.argv[2])
    elif cmd == "results":
        results()
    elif cmd == "wait":
        wait(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 120)
    else:
        print("unbekannt:", cmd)
        sys.exit(2)
