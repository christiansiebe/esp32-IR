#!/usr/bin/env python3
"""Selbstheilender Supervisor fuer den Sandbox-Kommando-Server.

Wird (idempotent) gestartet und haelt sandbox_term_server.py dauerhaft am Leben:

  - laeuft schon ein Supervisor -> sofort beenden (kein Doppelstart)
  - startet den Server als Kindprozess und startet ihn bei Absturz neu (Backoff)
  - Token wird vom Server PRO VERBINDUNG aus remote-shell/token gelesen ->
    Rotation = Datei ueberschreiben, KEIN Neustart noetig
  - Log:   remote-shell/run/server.log
  - PID:   remote-shell/run/supervisor.pid   (fuer `--status`/`--stop`)
  - URL:   aus $RUNTIME_ID abgeleitet (Override: SANDBOX_TERM_URL)

Aufruf:
    python3 /workspace/remote-shell/supervise.py            # startet (idempotent)
    python3 /workspace/remote-shell/supervise.py --status   # Status anzeigen
    python3 /workspace/remote-shell/supervise.py --stop     # stoppen
"""
import os
import signal
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.abspath(__file__))
RUN = os.path.join(BASE, "run")
TOKEN_FILE = os.environ.get("SANDBOX_TERM_TOKEN_FILE", os.path.join(BASE, "token"))
PORT = os.environ.get("SANDBOX_TERM_PORT", "12001")
PIDFILE = os.path.join(RUN, "supervisor.pid")
SRVPID = os.path.join(RUN, "server.pid")
LOGFILE = os.path.join(RUN, "server.log")
STOP = False


def single_instance():
    """Atomarer Lock gegen Doppelstart (fcntl.flock)."""
    import fcntl
    os.makedirs(RUN, exist_ok=True)
    fh = open(os.path.join(RUN, "supervisor.lock"), "w")
    try:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return None
    fh.write(str(os.getpid()))
    fh.flush()
    return fh


def alive(pid):
    """True nur, wenn der Prozess existiert UND kein Zombie ist.

    In diesem Sandbox-Container reapt PID 1 (openhands-agent-server) keine
    Kinder, getoetete Daemons bleiben als Zombie stehen. os.kill(pid, 0)
    gelingt dann trotzdem -> Zustand aus /proc/<pid>/stat pruefen.
    """
    try:
        with open(f"/proc/{pid}/stat") as fh:
            state = fh.read().rsplit(")", 1)[1].split()[0]
        return state != "Z"
    except (FileNotFoundError, IndexError, ProcessLookupError):
        return False


def read_pid(path):
    try:
        return int(open(path).read().strip())
    except (OSError, ValueError):
        return None


def log(msg):
    os.makedirs(RUN, exist_ok=True)
    with open(LOGFILE, "a") as fh:
        fh.write(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {msg}\n")


def token():
    tok = os.environ.get("SANDBOX_TERM_TOKEN", "")
    if not tok and os.path.exists(TOKEN_FILE):
        tok = open(TOKEN_FILE).read().strip()
    return tok


def url():
    explicit = os.environ.get("SANDBOX_TERM_URL", "")
    if explicit:
        return explicit
    rid = os.environ.get("RUNTIME_ID", "")
    return f"wss://work-2-{rid}.prod-runtime.all-hands.dev/" if rid else "<unknown>"


def on_term(*_):
    global STOP
    STOP = True


def ensure_deps():
    """websockets liegt im fluechtigen Overlay und verschwindet bei jedem
    Sandbox-Reset -> vor dem Start automatisch nachinstallieren."""
    try:
        import websockets  # noqa: F401
        return
    except ImportError:
        log("websockets fehlt - installiere nach")
    for args in (["--user"], []):
        r = subprocess.run(
            [sys.executable, "-m", "pip", "install", "--quiet", *args, "websockets"],
            capture_output=True, text=True,
        )
        if r.returncode == 0:
            log("websockets installiert")
            return
    log(f"websockets-Installation fehlgeschlagen: {r.stderr.strip()[:200]}")


def ensure_memory_link():
    """~/.openhands/memory/MEMORY.md ist das User-Memory (jedem Chat injiziert),
    liegt aber im fluechtigen Home. Auf das persistente, geteilte
    /workspace/MEMORY.md verlinken -> alle Chats der Sandbox schreiben in EINE
    Datei. Nach einem Reset hier neu setzen (idempotent)."""
    shared = "/workspace/MEMORY.md"
    link_dir = os.path.expanduser("~/.openhands/memory")
    link = os.path.join(link_dir, "MEMORY.md")
    try:
        if not os.path.exists(shared):
            return
        os.makedirs(link_dir, exist_ok=True)
        if os.path.islink(link):
            if os.readlink(link) == shared:
                return
            os.remove(link)
        elif os.path.exists(link):
            # echte Datei vorhanden -> sichern, nicht ueberschreiben
            os.rename(link, link + ".bak")
        os.symlink(shared, link)
        log("User-Memory-Symlink gesetzt -> /workspace/MEMORY.md")
    except OSError as exc:
        log(f"Memory-Symlink fehlgeschlagen: {exc}")


def ensure_memory_sync():
    """Haelt /workspace/MEMORY.md aktuell (Quelle: GitHub). Der Pull-Loop laeuft
    ohne Token (oeffentliches Repo). Single-Instance via PID-Datei; idempotent."""
    script = os.path.join(BASE, "sync-memory.sh")
    if not os.path.exists(script):
        return
    pidfile = os.path.join(RUN, "memory-sync.pid")
    pid = read_pid(pidfile)
    if pid and alive(pid):
        return
    try:
        log("starte memory-sync loop (GitHub -> /workspace/MEMORY.md)")
        proc = subprocess.Popen(
            ["bash", script, "loop"],
            stdout=open(os.path.join(RUN, "memory-sync.log"), "a"),
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        with open(pidfile, "w") as fh:
            fh.write(str(proc.pid))
    except OSError as exc:
        log(f"memory-sync Start fehlgeschlagen: {exc}")


def ensure_registration():
    """Diese Sandbox im GitHub-Register eintragen, damit der Odroid sie findet.
    Best-effort: braucht $RUNTIME_ID und einen Schreib-Token (Cloud-Secret
    GITHUB_WRITE_TOKEN). Fehler duerfen den Kanal nie blockieren."""
    script = os.path.join(BASE, "register_sandbox.py")
    if not os.path.exists(script):
        return
    try:
        r = subprocess.run(
            [sys.executable, script], capture_output=True, text=True, timeout=60,
        )
        log("register: " + (r.stdout.strip() or r.stderr.strip() or "ok")[:200])
    except Exception as exc:  # noqa: BLE001
        log(f"register fehlgeschlagen: {exc}")


def supervise():
    os.makedirs(RUN, exist_ok=True)
    ensure_deps()
    ensure_memory_link()
    ensure_memory_sync()
    ensure_registration()
    env = dict(os.environ)
    env["SANDBOX_TERM_TOKEN_FILE"] = TOKEN_FILE
    env["SANDBOX_TERM_PORT"] = PORT
    delay = 1
    while not STOP:
        if not token():
            log("kein Token - warte 30s")
            time.sleep(30)
            continue
        log("starte sandbox_term_server.py")
        proc = subprocess.Popen(
            [sys.executable, os.path.join(BASE, "sandbox_term_server.py")],
            stdout=open(LOGFILE, "a"),
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
        with open(SRVPID, "w") as fh:
            fh.write(str(proc.pid))
        started = time.time()
        while not STOP and proc.poll() is None:
            time.sleep(2)
        if STOP:
            proc.terminate()
            break
        if time.time() - started > 60:
            delay = 1
        log(f"Server beendet (rc={proc.returncode}) - Neustart in {delay}s")
        time.sleep(delay)
        delay = min(delay * 2, 30)
    log("Supervisor beendet")


def status():
    pid = read_pid(PIDFILE)
    if pid and alive(pid):
        print(f"Supervisor laeuft (PID {pid})")
        print(f"Proxy-URL fuer den Armbian-Agenten: {url()}")
    else:
        print("Supervisor laeuft NICHT")
    return 0 if pid and alive(pid) else 1


def stop():
    pid = read_pid(PIDFILE)
    spid = read_pid(SRVPID)
    if not (pid and alive(pid)):
        print("Supervisor laeuft nicht.")
        return 1
    os.kill(pid, signal.SIGTERM)
    for _ in range(20):
        if not alive(pid):
            break
        time.sleep(0.3)
    if spid and alive(spid):
        try:
            os.kill(spid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    print(f"Supervisor gestoppt (PID {pid}).")
    return 0


def main():
    if "--status" in sys.argv:
        raise SystemExit(status())
    if "--stop" in sys.argv:
        raise SystemExit(stop())

    existing = read_pid(PIDFILE)
    if existing and alive(existing):
        print(f"Supervisor laeuft bereits (PID {existing}) - nichts zu tun.")
        print(f"Proxy-URL fuer den Armbian-Agenten: {url()}")
        return
    if not token():
        raise SystemExit("kein Token (remote-shell/token fehlt/leer) - Abbruch.")

    os.makedirs(RUN, exist_ok=True)
    pid = os.fork()
    if pid > 0:
        time.sleep(0.6)
        newpid = read_pid(PIDFILE)
        if newpid and alive(newpid):
            print(f"Supervisor gestartet (PID {newpid}), Port {PORT}")
            print(f"Proxy-URL fuer den Armbian-Agenten: {url()}")
        else:
            print("Start fehlgeschlagen - siehe run/server.log")
        return

    # Kindprozess: eigener Session-Leader, haelt den Lock, supervidiert.
    os.setsid()
    lock = single_instance()          # noqa: F841  (Referenz muss leben)
    if lock is None:
        os._exit(0)                   # anderer Supervisor laeuft bereits
    open(PIDFILE, "w").write(str(os.getpid()))
    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)
    supervise()
    os._exit(0)


if __name__ == "__main__":
    main()
