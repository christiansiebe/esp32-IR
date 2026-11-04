#!/usr/bin/env python3
"""board_guard.py - EIN Ressourcen-/Stabilitaets-Waechter fuer den Odroid N2+.

Ersetzt drei Einzelwaechter, die sich gegenseitig ins Gehege kamen:
  * resource_guard.py    - tötete JEDEN Prozess ab 800 MB RSS. Der agent-server
                           braucht aber im Normalbetrieb 830-900 MB -> er wurde
                           im Betrieb regelmaessig abgeschossen (Chat tot).
  * ssd_fresser_guard.py - Sonderfall davon (nur "forensic_*").
  * tmp_guard.py         - /tmp (tmpfs) aufraeumen.

Leitsatz: ZUERST EINDAEMMEN, DANN TOETEN.
  * Ein Dienst wird NIE allein wegen seiner Groesse getoetet. Der agent-server
    ist legitim der groesste Verbraucher.
  * Getoetet bzw. neu gestartet wird nur:
      a) ein Dienst, dessen Health-Endpunkt nicht mehr antwortet (Haenger)
         -> `systemctl restart` (systemd eskaliert selbst SIGTERM -> SIGKILL),
      b) der EINZIGE groesste, nicht geschuetzte Prozess - und nur bei echter,
         SYSTEMWEITER Speichernot (MemAvailable < hart).
  * /tmp (tmpfs) wird erst ueber der Schwelle aufgeraeumt.

Die Daempfung selbst (MemoryHigh/MemoryMax/MemorySwapMax) macht systemd per
Drop-in; dieser Waechter ist das Sicherheitsnetz, nicht die erste Stufe.

Konfiguration: /etc/default/board-guard
Aufruf:
  board_guard.py                 Dauerlauf (systemd Type=simple)
  board_guard.py --once          ein Durchlauf
  board_guard.py --once --dry-run
  board_guard.py --status
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

CONF = "/etc/default/board-guard"
LOG = "/var/log/board-guard.log"
STATE = "/var/lib/board-guard/state.json"

DEFAULTS = {
    # Health: "unit|url|timeout_s", mehrere durch Komma getrennt.
    "HEALTH": "openhands-agent|http://127.0.0.1:8000/api/profiles|12",
    "HEALTH_FAILS": "3",          # so viele Fehlschlaege in Folge -> Neustart
    "HEALTH_COOLDOWN_S": "180",   # Mindestabstand zwischen zwei Neustarts
    # Ein NICHT laufender Dienst (failed/gestoppt) wird ebenfalls neu gestartet.
    # Ohne das blieb der Agent nach einem Absturz dauerhaft tot: die alte
    # Logik uebersprang inaktive Units, statt sie zu wecken.
    "HEALTH_RESTART_INACTIVE": "1",
    # Schonfrist nach dem Boot: so viele Sekunden nach Systemstart wird nicht
    # eingegriffen (sonst kaempft der Waechter mit dem Bootvorgang).
    "HEALTH_BOOT_GRACE_S": "180",
    # Wartungs-Sperre: existiert diese Datei, wird NICHT neu gestartet
    # (z. B. prepare_nvme_removal.sh stoppt den Dienst absichtlich).
    "HEALTH_HOLD_FILE": "/run/board-guard-hold",
    # Harter Deckel je EINZELNEM Prozess (unabhaengig von Systemnot): ein
    # entlaufener Kindprozess (z. B. ein Scan) wird gekillt, BEVOR er das
    # Board ausbremst. Geschuetzte Prozesse (agent-server, HA) sind ausgenommen.
    "PER_PROC_HARD_MB": "1600",
    # Systemweite Speichernot - NICHT pro Prozess.
    "MEM_AVAIL_HARD_MB": "150",   # darunter: groessten Ausreisser toeten
    "MEM_KILL_FLOOR_MB": "400",   # kleinere Prozesse nie toeten
    "KILL_GRACE_S": "3",          # SIGTERM -> SIGKILL
    "KILL_COOLDOWN_S": "120",
    # Diese Prozesse werden nie getoetet (Teilstring im Namen).
    # agent-server/agent-ui = die Arbeit und Oberflaeche des Nutzers: haben
    # absoluten Vorrang und duerfen nie von einem Waechter beendet werden.
    # homeassistant/supervisor = das eigentliche System des Nutzers: nie
    # anfassen, sonst faellt die Hausautomatisierung aus.
    "PROTECT": ("systemd sshd dockerd containerd Xorg Xtigervnc earlyoom "
                "hassio_supervisor supervisor homeassistant board_guard init "
                "kthreadd agent-server agent-ui cmd_agent sandbox_term vnc"),
    # tmpfs aufraeumen
    "TMP_PATH": "/tmp",
    "TMP_PATTERNS": "*.ppm *.pnm *.pgm *.pbm *.pam",
    "TMP_EXCLUDE": "",
    "TMP_HIGH_PCT": "50",
    "TMP_TARGET_PCT": "30",
    "TMP_MIN_AGE_S": "3600",
    "TMP_MIN_SIZE_MB": "20",
    "TMP_CRITICAL_PCT": "80",
    "TMP_CRITICAL_AGE_S": "30",
    "INTERVAL_S": "15",
}


# ----------------------------------------------------------------- Konfiguration
def load_conf(path=CONF, defaults=None):
    cfg = dict(defaults or DEFAULTS)
    try:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    return cfg


def as_list(value):
    """Wandelt "a b c" oder "a,b" in eine Liste."""
    return [x for x in value.replace(",", " ").split() if x]


def parse_health(value):
    """'unit|url|timeout' -> (unit, url, timeout). Ohne Timeout -> 12."""
    out = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        parts = [p.strip() for p in item.split("|")]
        if len(parts) < 2:
            continue
        unit, url = parts[0], parts[1]
        timeout = float(parts[2]) if len(parts) > 2 else 12.0
        out.append((unit, url, timeout))
    return out


# ----------------------------------------------------------------------- Log
def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%dT%H:%M:%S"), msg)
    print(line, flush=True)
    try:
        with open(LOG, "a") as fh:
            fh.write(line + "\n")
    except OSError:
        pass


# ------------------------------------------------------------------- Speicher
def read_meminfo(path="/proc/meminfo"):
    d = {}
    try:
        with open(path) as fh:
            for line in fh:
                if ":" in line:
                    k, v = line.split(":", 1)
                    d[k.strip()] = int(v.split()[0])
    except (OSError, ValueError):
        pass
    return d


def mem_available_mb(meminfo=None):
    mi = meminfo if meminfo is not None else read_meminfo()
    return mi.get("MemAvailable", mi.get("MemFree", 0)) // 1024


def read_processes():
    """[(pid, rss_mb, name)] aus ps."""
    try:
        out = subprocess.check_output(
            ["ps", "axo", "pid=,rss=,comm="], text=True, timeout=5)
    except Exception:
        return []
    procs = []
    for line in out.strip().splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        try:
            pid = int(parts[0])
            rss_mb = int(parts[1]) // 1024
        except ValueError:
            continue
        name = os.path.basename(parts[2].split(" ")[0])
        procs.append((pid, rss_mb, name))
    return procs


def is_protected(name, protect):
    return any(p and p in name for p in protect)


def pick_victim(procs, protect, floor_mb, hard_mb=None):
    """Groesster nicht geschuetzter Prozess, der getoetet werden soll.

    Zwei Gruende (der erste greift zuerst):
      * RSS >= hard_mb  -> harter Deckel je Prozess (entlaufener Kindprozess),
        unabhaengig von der Systemnot.
      * RSS >= floor_mb -> nur bei Systemnot (Aufrufer entscheidet das).

    Reine Funktion (testbar). Rueckgabe: (pid, rss_mb, name) oder None.
    """
    best = None
    for pid, rss_mb, name in procs:
        if is_protected(name, protect):
            continue
        if hard_mb is not None and rss_mb >= hard_mb:
            return (pid, rss_mb, name)
        if rss_mb < floor_mb:
            continue
        if best is None or rss_mb > best[1]:
            best = (pid, rss_mb, name)
    return best


# ---------------------------------------------------------------- Health-Check
def unit_active(unit):
    try:
        out = subprocess.run(
            ["systemctl", "is-active", unit], capture_output=True,
            text=True, timeout=5)
        return out.stdout.strip() == "active"
    except Exception:
        return False


def unit_should_run(unit):
    """True, wenn die Unit aktiv sein SOLLTE, aber nicht laeuft.

    failed -> ja (Absturz). inactive -> nur wenn sie nicht bewusst gestoppt
    wurde (dann ist sie 'inactive' und 'enabled' bleibt bestehen). Wir pruefen
    daher nur den Zustand 'failed' sowie 'inactive+enabled'.
    """
    try:
        out = subprocess.run(
            ["systemctl", "show", unit, "-p", "ActiveState", "-p", "UnitFileState"],
            capture_output=True, text=True, timeout=5)
    except Exception:
        return False
    state = {}
    for line in out.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            state[k.strip()] = v.strip()
    if state.get("ActiveState") == "failed":
        return True
    return (state.get("ActiveState") == "inactive"
            and state.get("UnitFileState") == "enabled")



def unit_active_since(unit):
    """Sekunden seit dem letzten Start des Dienstes, None bei Fehler."""
    try:
        out = subprocess.run(
            ["systemctl", "show", unit, "-p", "ActiveEnterTimestampMonotonic"],
            capture_output=True, text=True, timeout=5)
        v = None
        for line in out.stdout.splitlines():
            if line.startswith("ActiveEnterTimestampMonotonic="):
                v = line.split("=", 1)[1].strip()
        if not v or v == "0":
            return None
        with open("/proc/uptime") as fh:
            up = float(fh.read().split()[0])
        return up - (int(v) / 1000000.0)
    except Exception:
        return None

def boot_time():
    """Sekunden seit Systemstart (aus /proc/stat btime), None bei Fehler."""
    try:
        with open("/proc/stat") as fh:
            for line in fh:
                if line.startswith("btime "):
                    return time.time() - int(line.split()[1])
    except (OSError, ValueError):
        pass
    return None


def url_ok(url, timeout):
    """True, wenn der Endpunkt innerhalb von `timeout` antwortet."""
    try:
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return 200 <= resp.status < 500
    except urllib.error.HTTPError as e:
        # 4xx = Prozess lebt und antwortet -> nicht gehaengt
        return e.code < 500
    except Exception:
        return False


# ------------------------------------------------------------------ tmpfs
def usage_pct(path):
    try:
        st = os.statvfs(path)
    except OSError:
        return 0.0
    if st.f_blocks == 0:
        return 0.0
    return 100.0 * (st.f_blocks - st.f_bfree) / st.f_blocks


def is_open(path):
    try:
        st = os.stat(path)
    except OSError:
        return False
    target = (st.st_dev, st.st_ino)
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        for sub in ("fd", "map_files"):
            d = "/proc/%s/%s" % (pid, sub)
            try:
                entries = os.listdir(d)
            except OSError:
                continue
            for e in entries:
                try:
                    s = os.stat(os.path.join(d, e))
                except OSError:
                    continue
                if (s.st_dev, s.st_ino) == target:
                    return True
    return False


def tmp_candidates(cfg, now=None):
    now = now if now is not None else time.time()
    path = cfg["TMP_PATH"]
    pct = usage_pct(path)
    critical = pct >= float(cfg["TMP_CRITICAL_PCT"])
    min_age = (int(cfg["TMP_CRITICAL_AGE_S"]) if critical
               else int(cfg["TMP_MIN_AGE_S"]))
    min_bytes = int(float(cfg["TMP_MIN_SIZE_MB"]) * 1024 * 1024)
    patterns = as_list(cfg["TMP_PATTERNS"])
    excludes = as_list(cfg["TMP_EXCLUDE"])
    found = []
    try:
        names = os.listdir(path)
    except OSError:
        return found
    for name in names:
        if any(fnmatch.fnmatch(name, p) for p in excludes):
            continue
        if not any(fnmatch.fnmatch(name, p) for p in patterns):
            continue
        full = os.path.join(path, name)
        try:
            st = os.lstat(full)
        except OSError:
            continue
        if not os.path.isfile(full):
            continue
        if st.st_size < min_bytes or now - st.st_mtime < min_age:
            continue
        if is_open(full):
            continue
        found.append((full, st.st_size))
    found.sort(key=lambda x: -x[1])
    return found


def clean_tmp(cfg, dry_run=False):
    pct = usage_pct(cfg["TMP_PATH"])
    high = float(cfg["TMP_HIGH_PCT"])
    if pct < high:
        return 0
    target = float(cfg["TMP_TARGET_PCT"])
    freed = removed = 0
    for full, size in tmp_candidates(cfg):
        if dry_run:
            log("[dry-run] tmp: wuerde loeschen %s (%.1f MB)"
                % (full, size / 1048576.0))
            continue
        try:
            os.unlink(full)
        except OSError as exc:
            log("tmp: Fehler beim Loeschen von %s: %s" % (full, exc))
            continue
        freed += size
        removed += 1
        if usage_pct(cfg["TMP_PATH"]) <= target:
            break
    if not dry_run and removed:
        log("tmp %.1f%% -> %.1f%%: %d Datei(en), %.1f MB frei"
            % (pct, usage_pct(cfg["TMP_PATH"]), removed, freed / 1048576.0))
    return removed


# ------------------------------------------------------------------ Zustand
def load_state():
    try:
        with open(STATE) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {"health_fails": {}, "last_restart": {}, "last_kill": 0.0}


def save_state(state):
    try:
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        tmp = STATE + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(state, fh)
        os.replace(tmp, STATE)
    except OSError:
        pass


# ------------------------------------------------------------------ Aktionen
def restart_unit(unit, action="restart"):
    log("[%s] %s -> systemctl %s" % (action.upper(), unit, action))
    try:
        subprocess.run(["systemctl", action, unit], timeout=90,
                       capture_output=True)
        return True
    except Exception as exc:
        log("[%s] %s fehlgeschlagen: %s" % (action.upper(), unit, exc))
        return False


def kill_pid(pid, name, reason, grace):
    log("[KILL] %s (pid %d) - %s" % (name, pid, reason))
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return True
    except PermissionError:
        log("[KILL] kein Recht auf pid %d" % pid)
        return False
    time.sleep(grace)
    try:
        os.kill(pid, 0)
        os.kill(pid, signal.SIGKILL)
        log("[KILL] %s (pid %d) nach Grace SIGKILL" % (name, pid))
        return True
    except ProcessLookupError:
        return True
    except PermissionError:
        return False


# ------------------------------------------------------------------ Durchlauf
def check_health(cfg, state, dry_run=False):
    fails_needed = int(cfg["HEALTH_FAILS"])
    cooldown = float(cfg["HEALTH_COOLDOWN_S"])
    restart_inactive = cfg.get("HEALTH_RESTART_INACTIVE", "1") not in ("0", "no", "false")
    hold_file = cfg.get("HEALTH_HOLD_FILE", "")
    grace = float(cfg.get("HEALTH_BOOT_GRACE_S", "0"))
    unit_grace = float(cfg.get("HEALTH_UNIT_GRACE_S", "0"))
    now = time.time()

    if hold_file and os.path.exists(hold_file):
        return
    up = boot_time()
    if up is not None and up < grace:
        return

    for unit, url, timeout in parse_health(cfg["HEALTH"]):
        if not unit_active(unit):
            # Dienst laeuft nicht: entweder Absturz (failed) oder bewusst
            # gestoppt. Frueher wurde das uebersprungen -> der Agent blieb
            # nach einem Absturz dauerhaft tot. Jetzt wird er geweckt.
            if not restart_inactive or not unit_should_run(unit):
                state["health_fails"][unit] = 0
                continue
            if now - state["last_restart"].get(unit, 0) < cooldown:
                log("health: %s nicht aktiv, Neustart unterdrueckt (Cooldown)"
                    % unit)
                continue
            if dry_run:
                log("[dry-run] health: %s nicht aktiv -> wuerde starten" % unit)
                continue
            log("health: %s nicht aktiv -> systemctl start" % unit)
            if restart_unit(unit, action="start"):
                state["last_restart"][unit] = now
                state["health_fails"][unit] = 0
            continue
        if not url:
            # Nur "laeuft der Dienst" pruefen (kein HTTP-Endpunkt vorhanden).
            state["health_fails"][unit] = 0
            continue
        if url_ok(url, timeout):
            if state["health_fails"].get(unit):
                log("health: %s wieder ok" % unit)
            state["health_fails"][unit] = 0
            continue
        since = unit_active_since(unit)
        if since is not None and since < unit_grace:
            log("health: %s erst %ds alt - Schonfrist %.0fs, kein Neustart"
                % (unit, int(since), unit_grace))
            state["health_fails"][unit] = 0
            continue
        n = state["health_fails"].get(unit, 0) + 1
        state["health_fails"][unit] = n
        log("health: %s antwortet nicht (%d/%d) - %s"
            % (unit, n, fails_needed, url))
        if n < fails_needed:
            continue
        if now - state["last_restart"].get(unit, 0) < cooldown:
            log("health: %s Neustart unterdrueckt (Cooldown)" % unit)
            continue
        if dry_run:
            log("[dry-run] health: wuerde %s neu starten" % unit)
            continue
        if restart_unit(unit, action="restart"):
            state["last_restart"][unit] = now
            state["health_fails"][unit] = 0


def check_memory(cfg, state, dry_run=False):
    protect = as_list(cfg["PROTECT"])
    floor = int(cfg["MEM_KILL_FLOOR_MB"])
    hard = int(cfg["PER_PROC_HARD_MB"])
    avail = mem_available_mb()
    now = time.time()
    cooldown = float(cfg["KILL_COOLDOWN_S"])
    procs = read_processes()

    # 1) Harter Deckel je Prozess - greift IMMER, auch ohne Systemnot.
    victim = pick_victim(procs, protect, floor, hard_mb=hard)
    if victim is not None and victim[1] >= hard:
        if now - state.get("last_kill", 0) < cooldown:
            return
        pid, rss, name = victim
        reason = "Prozess-Deckel %d MB erreicht (RSS %d MB)" % (hard, rss)
        if dry_run:
            log("[dry-run] mem: wuerde %s (pid %d, %d MB) toeten - %s"
                % (name, pid, rss, reason))
            return
        if kill_pid(pid, name, reason, float(cfg["KILL_GRACE_S"])):
            state["last_kill"] = now
        return

    # 2) Systemweite Speichernot - groessten Ausreisser toeten.
    avail_hard = int(cfg["MEM_AVAIL_HARD_MB"])
    if avail >= avail_hard:
        return
    if now - state.get("last_kill", 0) < cooldown:
        return
    if victim is None:
        log("mem: nur %d MB frei, aber kein Ausreisser ueber %d MB"
            % (avail, floor))
        return
    pid, rss, name = victim
    reason = "System-RAM %d MB < %d MB, groesster Ausreisser" % (avail, avail_hard)
    if dry_run:
        log("[dry-run] mem: wuerde %s (pid %d, %d MB) toeten" % (name, pid, rss))
        return
    if kill_pid(pid, name, reason, float(cfg["KILL_GRACE_S"])):
        state["last_kill"] = now


def run_once(cfg, state, dry_run=False):
    check_health(cfg, state, dry_run)
    check_memory(cfg, state, dry_run)
    clean_tmp(cfg, dry_run)


def cmd_status(cfg):
    print("MemAvailable: %d MB (hart %s MB)"
          % (mem_available_mb(), cfg["MEM_AVAIL_HARD_MB"]))
    print("tmp %s: %.1f%% (Schwelle %s%%)"
          % (cfg["TMP_PATH"], usage_pct(cfg["TMP_PATH"]), cfg["TMP_HIGH_PCT"]))
    for unit, url, timeout in parse_health(cfg["HEALTH"]):
        active = unit_active(unit)
        if not url:
            print("health %-20s active=%-6s (nur Dienst-Status)" % (unit, active))
            continue
        ok = url_ok(url, timeout) if active else False
        print("health %-20s active=%-6s url_ok=%s" % (unit, active, ok))
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args(argv)

    cfg = load_conf()
    if args.status:
        return cmd_status(cfg)

    state = load_state()
    if args.once:
        run_once(cfg, state, args.dry_run)
        if not args.dry_run:
            save_state(state)
        return 0

    log("=== board-guard start (v1) ===")
    interval = float(cfg["INTERVAL_S"])
    while True:
        try:
            run_once(cfg, state, args.dry_run)
            save_state(state)
        except Exception as exc:      # nie sterben
            log("[fehler] %s" % exc)
        time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
