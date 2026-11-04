#!/usr/bin/env python3
"""Preflight-Audit: prueft die bekannten Fehlerbilder PROAKTIV (vor dem Ausfall).

Jeder Check liefert OK / WARN / FAIL. Exit 1, wenn etwas FAIL ist (fuer Timer/Alarm).
Keine Aenderungen - nur lesen und berichten.
"""
import json, os, shutil, subprocess, socket, sys, time

RES = []
def add(name, status, detail=""):
    RES.append((status, name, detail))

def run(cmd, t=15):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=t)
    except Exception as e:
        class R: returncode=1; stdout=""; stderr=str(e)
        return R()

def check_ports_multiport():
    # iptables --multiport deckelt bei 15 Ports -> stille Regel-Ablehnung
    try:
        line = [l for l in open("/etc/default/fw-rules") if l.startswith("LAN_TCP_PORTS=")]
        ports = line[0].split("=",1)[1].strip().split(",") if line else []
        ports = [p for p in ports if p]
    except Exception:
        ports = []
    if len(ports) > 15:
        # nur FAIL, wenn der Code nicht splittet
        src = open("/usr/local/sbin/fw_rules.py").read()
        if "blocks(parse_ports" in src or "i:i + 15" in src:
            add("fw multiport", "OK", "%d Ports (Code splittet in <=15er-Bloecke)" % len(ports))
        else:
            add("fw multiport", "FAIL", "%d Ports > 15 UND Code splittet nicht -> LAN-Lockout" % len(ports))
    else:
        add("fw multiport", "OK", "%d Ports" % len(ports))

def check_fw_rules_active():
    # Jede LAN-ACCEPT-Regel muss WIRKLICH in der Kette sitzen
    r = run(["iptables","-S","FW"])
    if r.returncode != 0:
        add("fw aktiv", "WARN", "keine FW-Kette (Firewall aus?)"); return
    has_lan = any("192.168." in l and "-p tcp" in l for l in r.stdout.splitlines())
    policy = run(["iptables","-S","INPUT"]).stdout.splitlines()
    is_drop = any(l.startswith("-P INPUT DROP") for l in policy)
    if is_drop and not has_lan:
        add("fw aktiv", "FAIL", "INPUT=DROP aber keine LAN-TCP-ACCEPT-Regel -> Lockout!")
    else:
        add("fw aktiv", "OK", "LAN-Regel vorhanden, INPUT=%s" % ("DROP" if is_drop else "ACCEPT"))

def check_services():
    want = ["nginx","openhands-agent","openhands-keybroker","board-guard"]
    for s in want:
        r = run(["systemctl","is-active",s])
        en = run(["systemctl","is-enabled",s]).stdout.strip()
        if r.stdout.strip() != "active":
            add("dienst "+s, "FAIL", "nicht aktiv")
        elif en not in ("enabled","static","enabled-runtime"):
            add("dienst "+s, "WARN", "aktiv aber NICHT enabled (Reboot!)")
        else:
            add("dienst "+s, "OK", "aktiv+enabled")

def check_key_files():
    for f in ("/etc/openhands/llm-key",):
        if not os.path.exists(f):
            add("key "+os.path.basename(f), "FAIL", "fehlt"); continue
        mode = oct(os.stat(f).st_mode & 0o777)
        size = os.path.getsize(f)
        if mode != "0o600":
            add("key "+os.path.basename(f), "WARN", "Rechte %s (soll 0600)" % mode)
        elif size < 10:
            add("key "+os.path.basename(f), "FAIL", "leer/zu kurz (%d B)" % size)
        else:
            add("key "+os.path.basename(f), "OK", "%d B, %s" % (size, mode))

def check_local_broker():
    try:
        tok = open("/etc/openhands/keybroker.tok").read().strip()
        import urllib.request
        r = urllib.request.Request("http://127.0.0.1:12099/current", headers={"Authorization":"Bearer "+tok})
        with urllib.request.urlopen(r, timeout=6) as x:
            k = x.read().decode().strip()
        add("key-broker", "OK" if k.startswith("sk-") else "FAIL", "liefert key" if k.startswith("sk-") else "kein sk- Key")
    except Exception as e:
        add("key-broker", "FAIL", str(e)[:60])

def check_dns():
    try:
        socket.setdefaulttimeout(4)
        ip = socket.gethostbyname("github.com")
        add("dns", "OK", "github.com -> %s" % ip)
    except Exception as e:
        add("dns", "FAIL", "Aufloesung fehlgeschlagen: %s" % str(e)[:40])

def check_disk():
    for path in ("/", "/mnt/ssd", "/root"):
        try:
            t,u,f = shutil.disk_usage(path)
            pct = u*100//t
            add("disk "+path, "OK" if pct < 85 else ("WARN" if pct < 92 else "FAIL"), "%d%% belegt" % pct)
        except Exception as e:
            add("disk "+path, "WARN", str(e)[:40])

def check_backups():
    # Schnell: nur die oberste Ebene + Alter, KEIN rekursiver walk (der kann
    # bei rsync-Historien minutenlang laufen und den Audit blockieren).
    cands = ["/mnt/ssd/backups", "/root/backups", "/var/backups", "/root/oh-backup-llmfix-20261103-224156"]
    found = next((c for c in cands if os.path.isdir(c)), None)
    if not found:
        add("backup", "WARN", "kein bekanntes Backup-Verzeichnis"); return
    r = run(["bash","-lc","find %s -maxdepth 2 -type f -printf '%%T@\n' 2>/dev/null | sort -rn | head -1" % found])
    try:
        newest = float((r.stdout or "0").strip() or 0)
    except Exception:
        newest = 0
    if newest == 0:
        add("backup", "WARN", "%s: keine Dateien in Ebene <=2" % found)
    else:
        age_h = (time.time()-newest)/3600
        add("backup", "OK" if age_h < 48 else "WARN", "juengstes Backup vor %.1f h (%s)" % (age_h, found))

def check_watchdog_gate():
    # Der Watchdog laeuft in der Sandbox; hier (Odroid) nur pruefen, ob die
    # Gate-Datei vorhanden ist. Fehlt sie, ist das kein Odroid-Fehler -> OK.
    for p in ("/workspace/remote-shell/buddy.py", "/root/esp32-IR/remote-shell/buddy.py"):
        if os.path.exists(p):
            src = open(p).read()
            ok = ("needs_wake" in src and "open_tasks" in src and "MAX_WAKES" in src)
            add("watchdog-gate", "OK" if ok else "FAIL",
                "weckt nur bei offener Aufgabe" if ok else "Gate fehlt -> Token-Bleed")
            return
    add("watchdog-gate", "OK", "kein Watchdog auf dem Odroid (laeuft in der Sandbox)")

def check_certs():
    # nginx-Zertifikate: Restlaufzeit
    r = run(["bash","-lc","for f in /etc/nginx/ssl/*.crt /etc/letsencrypt/live/*/fullchain.pem; do [ -f \"$f\" ] && echo $f; done 2>/dev/null"])
    certs = [l for l in r.stdout.splitlines() if l.strip()]
    if not certs:
        add("certs", "WARN", "keine Zertifikate gefunden"); return
    worst = 999
    for c in certs:
        e = run(["openssl","x509","-enddate","-noout","-in",c])
        if e.returncode != 0: continue
        try:
            import datetime
            d = e.stdout.split("=",1)[1].strip()
            exp = datetime.datetime.strptime(d, "%b %d %H:%M:%S %Y %Z")
            days = (exp - datetime.datetime.utcnow()).days
            worst = min(worst, days)
        except Exception: pass
    if worst == 999:
        add("certs", "WARN", "Restlaufzeit nicht lesbar")
    else:
        add("certs", "OK" if worst > 14 else ("WARN" if worst > 3 else "FAIL"), "naechstes laeuft in %d Tagen ab" % worst)

def main():
    for fn in (check_ports_multiport, check_fw_rules_active, check_services,
               check_key_files, check_local_broker, check_dns, check_disk,
               check_backups, check_watchdog_gate, check_certs):
        try: fn()
        except Exception as e: add(fn.__name__, "WARN", "Check-Fehler: %s" % str(e)[:50])
    fails = [r for r in RES if r[0]=="FAIL"]
    warns = [r for r in RES if r[0]=="WARN"]
    for st,name,det in RES:
        print("%-5s %-22s %s" % (st, name, det))
    print("\n%d OK, %d WARN, %d FAIL" % (len(RES)-len(fails)-len(warns), len(warns), len(fails)))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
