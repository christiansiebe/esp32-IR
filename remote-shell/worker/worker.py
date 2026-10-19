#!/usr/bin/env python3
"""Odroid-Worker: arbeitet eine lokale Task-Queue ab.

Ein Worker = eine lokale OpenHands-Conversation (oh -s <name>) auf dem
Odroid-Agent-Server (127.0.0.1:8000). Das kostet KEIN Cloud-Guthaben:
es entsteht keine neue Cloud-Sandbox und keine Cloud-Conversation.

Verzeichnisse (Default /root/worker):
  queue/<id>.task     eingehende Auftraege (JSON {text, worker, submitted_by, ts})
  running/<id>.task   gerade in Arbeit (atomar per rename beansprucht)
  results/<id>.json   Ergebnis {id, worker, status, output, started, finished}
  worker.log          Protokoll

Mehrere Worker teilen sich EINE Queue: wer frei ist, greift den aeltesten
unbeanspruchten Task (os.rename ist atomar -> kein Doppelgriff).

Aufruf: worker.py --name worker1 [--dir /root/worker] [--fresh] [--timeout 3600]
"""
import argparse
import fcntl
import glob
import json
import os
import subprocess
import time
import urllib.request

OH = os.environ.get("WORKER_OH_BIN", "/usr/local/bin/oh")

# Auto-Beauftragung: die Worker holen neue Jobs selbst aus der GitHub-Job-Queue
# (raw, oeffentlich -> read-only, kein Token auf dem Odroid). Parents schreiben
# Jobs mit worker_enqueue.py. Lokale queue/*.task funktioniert weiterhin.
JOBS_RAW = os.environ.get(
    "WORKER_JOBS_RAW",
    "https://raw.githubusercontent.com/christiansiebe/esp32-IR/"
    "cline/power-history-diag/remote-shell/worker/jobs.json")


def fetch_jobs():
    """GitHub-Job-Queue lesen. Fehler still ignorieren (Kanal darf nie scheitern)."""
    try:
        with urllib.request.urlopen(JOBS_RAW, timeout=15) as r:
            return json.loads(r.read()).get("jobs", {})
    except Exception:
        return {}


def pull_jobs(d, logf):
    """Neue Jobs aus GitHub in die lokale Queue legen (idempotent).

    WICHTIG: Der ganze Vorgang laeuft unter einem flock — sonst lesen zwei
    Worker gleichzeitig ein noch unbekanntes Job und legen ihn DOPPELT in die
    Queue (beide fuehren ihn dann aus). seen.json verhindert zusaetzlich, dass
    ein bereits bekannter Job (auch nach Neustart) erneut eingestellt wird."""
    lock_path = os.path.join(d, ".jobs.lock")
    with open(lock_path, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            seen_path = os.path.join(d, "seen.json")
            try:
                seen = json.load(open(seen_path))
            except (ValueError, OSError):
                seen = {}
            changed = False
            for jid, j in fetch_jobs().items():
                if jid in seen:
                    continue
                text = (j.get("text") or "").strip()
                if text:
                    with open(os.path.join(d, "queue", f"gh-{jid}.task"), "w") as fh:
                        json.dump({"text": text, "submitted_by": j.get("by", "github"),
                                   "gh_job": jid}, fh)
                    log(f"[github] Job {jid} in die Queue gelegt", logf)
                seen[jid] = j.get("state", "open")
                changed = True
            if changed:
                with open(seen_path, "w") as fh:
                    json.dump(seen, fh)
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)



def log(msg, path):
    line = f"{time.strftime('%Y-%m-%dT%H:%M:%S')} {msg}"
    print(line, flush=True)
    try:
        with open(path, "a") as fh:
            fh.write(line + "\n")
    except OSError:
        pass


def claim(d):
    """Aeltesten unbeanspruchten Task atomar beanspruchen.

    os.rename ist atomar: greifen zwei Worker denselben Task, gewinnt genau
    einer; der andere bekommt OSError und nimmt den naechsten. Zusaetzlich
    werden Tasks uebersprungen, die schon laufen oder fertig sind (Schutz vor
    doppelter Ausfuehrung, falls doch mal eine Kopie in der Queue landet)."""
    for t in sorted(glob.glob(os.path.join(d, "queue", "*.task"))):
        tid = os.path.basename(t)[:-5]
        if os.path.exists(os.path.join(d, "running", f"{tid}.task")):
            continue
        if os.path.exists(os.path.join(d, "results", f"{tid}.json")):
            try:
                os.remove(t)
            except OSError:
                pass
            continue
        dst = os.path.join(d, "running", f"{tid}.task")
        try:
            os.rename(t, dst)
            # PID/Startzeit stempeln, damit ein Neustart erkennen kann, ob der
            # Task wirklich verwaist ist (sonst laeuft er noch in einem anderen
            # Worker, der sich dieselbe Queue teilt).
            try:
                with open(os.path.join(d, "running", f"{tid}.pid"), "w") as fh:
                    json.dump({"pid": os.getpid(), "ts": time.time()}, fh)
            except OSError:
                pass
            return tid, dst
        except OSError:
            continue
    return None, None


def _pid_is_worker(pid):
    """Laeuft unter dieser PID noch ein Worker-Prozess?"""
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as fh:
            return "worker.py" in fh.read().decode(errors="replace")
    except OSError:
        return False


def requeue_orphans(d, logf):
    """Verwaiste running-Tasks (Worker-Neustart/-absturz) zurueck in die Queue.

    Ein Task in running/ kann nur von einem Neustart/Absturz stammen: der
    ausfuehrende Worker-Prozess ist dann tot. Lebt die gestempelte PID noch in
    einem Worker, bleibt der Task unangetastet (die Queue wird von mehreren
    Workern geteilt -> kein Doppelgriff). Fehlt der Stempel (Alt-Task), wird er
    ebenfalls zurueckgestellt."""
    rdir = os.path.join(d, "running")
    requeued = 0
    for t in sorted(glob.glob(os.path.join(rdir, "*.task"))):
        tid = os.path.basename(t)[:-5]
        meta = os.path.join(rdir, f"{tid}.pid")
        pid = None
        try:
            pid = json.load(open(meta)).get("pid")
        except (ValueError, OSError):
            pass
        if pid and pid != os.getpid() and _pid_is_worker(pid):
            continue  # laeuft noch -> nicht anfassen
        dst = os.path.join(d, "queue", f"{tid}.task")
        try:
            os.rename(t, dst)
        except OSError:
            continue
        try:
            os.remove(meta)
        except OSError:
            pass
        requeued += 1
        log(f"[requeue] verwaister Task {tid} zurueck in die Queue", logf)
    return requeued


def run_oh(name, text, fresh, timeout):
    cmd = [OH, "-s", name]
    if fresh:
        cmd.append("-n")
    cmd.append(text)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired as e:
        out = e.stdout or ""
        if isinstance(out, bytes):
            out = out.decode(errors="replace")
        return None, f"TIMEOUT nach {timeout}s\n{out}"
    except OSError as e:
        return None, f"STARTFEHLER: {e}"


# Exit-Codes, die `oh` fuer wiederholbare Fehler liefert (siehe oh.sh):
#   3 = transienter LLM-Fehler, 5 = keine Antwort im Zeitfenster
RETRYABLE_EXIT_CODES = {3, 5}
# Textmuster, die auf einen wiederholbaren LLM-Fehler hindeuten (falls `oh`
# eine aeltere Version ist oder der Fehler anders formuliert ankommt).
RETRYABLE_HINTS = (
    "missing credentials", "litellm_proxyexception", "internalservererror",
    "llmserviceunavailableerror", "rate limit", "temporarily unavailable",
    "service unavailable", "502", "503", "504", "keine antwort",
)
WORKER_RETRIES = int(os.environ.get("WORKER_RETRIES", "3"))
WORKER_RETRY_BASE = float(os.environ.get("WORKER_RETRY_BASE", "10"))


def is_retryable(rc, out):
    """Transienter/retrybarer LLM-Fehler statt echtem Task-Fehler?"""
    if rc in RETRYABLE_EXIT_CODES:
        return True
    low = (out or "").lower()
    return any(h in low for h in RETRYABLE_HINTS)


def run_task(name, text, fresh, timeout, logf=None):
    """Task ausfuehren; bei transientem LLM-Fehler mit Backoff wiederholen.

    Ein transienter Fehler ("Missing credentials" am LiteLLM-Proxy) darf den
    Task NICHT als fehlgeschlagen verbuchen: er wird mit frischer Conversation
    und wachsender Wartezeit erneut versucht.
    """
    rc, out = run_oh(name, text, fresh, timeout)
    # Sofortiger Einzel-Retry, wenn die Conversation verklemmt ist (bisheriges
    # Verhalten, jetzt nur noch wenn noch keine Wiederholung noetig war).
    if rc != 0 and not fresh and not is_retryable(rc, out):
        rc2, out2 = run_oh(name, text, True, timeout)
        if rc2 == 0:
            return rc2, out2 + "\n[Retry mit frischer Conversation erfolgreich]"
        rc, out = rc2, out2
    attempt = 0
    while is_retryable(rc, out) and attempt < WORKER_RETRIES:
        attempt += 1
        wait = WORKER_RETRY_BASE * (2 ** (attempt - 1))
        if logf:
            log(f"[{name}] transienter LLM-Fehler (rc={rc}); "
                f"Versuch {attempt}/{WORKER_RETRIES} in {wait:.0f}s", logf)
        time.sleep(wait)
        # Eine an einen veralteten Schluessel gebundene Conversation heilt nie
        # -> frische Conversation erzwingen.
        rc, out = run_oh(name, text, True, timeout)
        if rc == 0:
            out += f"\n[Erfolg nach {attempt} Wiederholung(en)]"
            break
    return rc, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--dir", default="/root/worker")
    ap.add_argument("--fresh", action="store_true",
                    help="pro Task eine neue Conversation (Default: wiederverwenden)")
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument("--poll", type=int, default=5)
    ap.add_argument("--jobs-poll", type=int, default=30,
                    help="Intervall fuer die GitHub-Job-Queue (0=aus)")
    a = ap.parse_args()

    for sub in ("queue", "running", "results"):
        os.makedirs(os.path.join(a.dir, sub), exist_ok=True)
    logf = os.path.join(a.dir, "worker.log")
    log(f"[{a.name}] gestartet (dir={a.dir} fresh={a.fresh} timeout={a.timeout}s "
        f"jobs-poll={a.jobs_poll}s)", logf)
    # Verwaiste Tasks aus running/ (Neustart/Absturz) wieder aufnehmen.
    requeue_orphans(a.dir, logf)

    last_jobs = 0.0
    while True:
        # Neue Jobs aus GitHub automatisch aufnehmen.
        if a.jobs_poll and time.time() - last_jobs >= a.jobs_poll:
            pull_jobs(a.dir, logf)
            last_jobs = time.time()
        tid, path = claim(a.dir)
        if not tid:
            time.sleep(a.poll)
            continue
        try:
            meta = json.load(open(path))
        except (ValueError, OSError):
            try:
                meta = {"text": open(path).read()}
            except OSError:
                meta = {"text": ""}
        text = (meta.get("text") or "").strip()
        log(f"[{a.name}] Task {tid} uebernommen: {text[:100]!r}", logf)
        started = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        rc, out = run_task(a.name, text, a.fresh, a.timeout, logf)
        result = {
            "id": tid, "worker": a.name,
            "status": "ok" if rc == 0 else "error", "returncode": rc,
            "output": out, "text": text,
            "submitted_by": meta.get("submitted_by"),
            "started": started,
            "finished": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        with open(os.path.join(a.dir, "results", f"{tid}.json"), "w") as fh:
            json.dump(result, fh, indent=2)
        try:
            os.remove(path)
        except OSError:
            pass  # anderer Worker hat die Datei schon entfernt -> kein Absturz
        try:
            os.remove(os.path.join(a.dir, "running", f"{tid}.pid"))
        except OSError:
            pass
        log(f"[{a.name}] Task {tid} fertig: {result['status']}", logf)


if __name__ == "__main__":
    main()
