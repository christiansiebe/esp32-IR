#!/usr/bin/env python3
"""Delegiert die offenen Odroid-Aufgaben an Sub-Agenten (eigene Cloud-Conversations).

Fleet-Regel (AGENTS.md, Branch feat/dfs-clock-visibility): jeder esp32-IR-Chat
arbeitet ueber Sub-Agenten; der Parent koordiniert und reviewt.

Board-Arbeit bleibt beim Parent: der remote-shell-Kanal ist eine GETEILTE
Verbindung und cmd_agent.py hat keine Sperre - zwei Agenten wuerden denselben
Befehl doppelt ausfuehren und das Board haengen. Deshalb steht in jedem Brief:
Board NICHT anfassen, nur Repo-/Host-Arbeit.

Jeder Sub-Agent: eigener Branch, PR gegen cline/power-history-diag, nie main.
"""
import json
import os
import sys
import time
import urllib.request

BASE = 'https://app.all-hands.dev'
KEY = os.environ.get('OPENHANDS_API_KEY') or os.environ.get('OPENHANDS_CLOUD_API_KEY')
REPO = 'christiansiebe/esp32-IR'
BRANCH = 'cline/power-history-diag'
PARENT = '7a894c13678043e58935dd1509217a75'

COMMON = """
WICHTIG - Randbedingungen (verbindlich):
- NICHT auf `main` pushen. Arbeite auf dem unten genannten NEUEN Branch und
  oeffne einen Pull Request GEGEN `cline/power-history-diag`.
- Fasse das Odroid-Board NICHT an und benutze NICHT den remote-shell-Kanal
  (remote-shell/cmd_send.sh, supervise.py, /dev/mem, systemctl am Board). Der
  Kanal ist eine GETEILTE Verbindung; parallele Nutzung fuehrt zu doppelt
  ausgefuehrten Befehlen und Board-Haengern. Board-Verifikation macht der Parent.
- Halte dich von Dateien fern, die andere Agenten besitzen (steht je Aufgabe).
- Committe frueh und oft (kleine Commits), damit bei einem Sandbox-Reset nichts
  verloren geht. Der Chat kann nach ~20 min Inaktivitaet zurueckgesetzt werden.
- Fuehre eine `TASKS.json` in deinem Workspace (ein Eintrag je Aufgabe,
  `"status": "open" | "done"`). Offene Eintraege = wachbleiben.
- Absturzsicherheit: Wenn ein Werkzeug/eine Abhaengigkeit fehlt, installiere es
  oder ueberspringe den Schritt mit Notiz - brich die Aufgabe NICHT deswegen ab.
  Beende mit einer kurzen Zusammenfassung (was fertig, was offen, Branch, PR-URL).
"""

JOBS = [
    dict(
        title='ODROID: noVNC extern erreichbar (Tunnel 12000)',
        branch='feat/novnc-external-tunnel',
        prompt="""Repo: christiansiebe/esp32-IR (Basis-Branch cline/power-history-diag).

Aufgabe: Den XFCE-Desktop des Odroid N2+ von AUSSERHALB des LAN erreichbar
machen - ueber den bestehenden Reverse-Tunnel zum Sandbox-Proxy (Port 12000).

Ist-Zustand (dokumentiert in `remote-shell/ha/novnc-README.md`):
- TigerVNC `:1` hoert NUR auf `127.0.0.1:5901`.
- noVNC/websockify auf dem Odroid: `0.0.0.0:6080`, per iptables-Kette `VNC_LAN`
  auf das eigene LAN beschraenkt. Im LAN: `http://odroidn2.local:6080/vnc.html`
- Es gibt bereits `remote-shell/ha/reverse_ssh.sh` (Odroid baut SSH-Tunnel zum
  Sandbox-Host) und der Sandbox-Proxy veroeffentlicht Port 12000 oeffentlich.

Ziel (reine Repo-/Host-Arbeit, KEIN Board-Zugriff):
1. `remote-shell/ha/novnc-external-README.md`: Design + Sicherheitsanalyse.
   Erklaere die Kette Browser -> Sandbox-Proxy :12000 -> (Reverse-Tunnel)
   -> Odroid :5901, und warum das ein OEFFENTLICH erreichbarer root-Desktop ist.
   Bewerte klar: braucht der externe Zugriff ein VNC-Passwort
   (`-SecurityTypes VncAuth`) plus zusaetzliche Auth am Proxy? Gib eine
   begruendete Empfehlung und nenne die Restrisiken.
2. `remote-shell/ha/start_novnc_bridge.sh`: startet im SANDBOX-Host ein
   `websockify` auf Port 12000, das auf den lokalen Endpunkt des Reverse-Tunnels
   zeigt. Idempotent, mit `--help` und Dry-Run, sauberer Fehler wenn der Tunnel
   fehlt. Kein Board-Zugriff.
3. Wenn sinnvoll ohne Board testbar: ein kleiner Host-Test fuer die Argument-
   und URL-Logik (`remote-shell/ha/test_novnc_bridge.py`, reine Funktionen,
   keine Mocks).

Abnahme: `sh -n` sauber; Test (falls vorhanden) gruen.
NICHT anfassen: `novnc.service`, `xfce-vnc.service`, `deploy_novnc.sh`,
`vnc_lan_guard.py`, `vnc-lan-guard.*`, `novnc-README.md`.
""",
    ),
    dict(
        title='ODROID: stabilen OC-Punkt (2496 MHz) als umkehrbares Overlay',
        branch='feat/oc-opp-overlay',
        prompt="""Repo: christiansiebe/esp32-IR (Basis-Branch cline/power-history-diag).

Aufgabe: Den gemessenen stabilen Overclock-Punkt (2496 MHz, big cluster) als
UMKEHRBARES Overlay bereitstellen - ohne das Board anzufassen.

Kontext (`remote-shell/ha/PLL-OC-README.md`, bereits gemessen):
- big-PLL: HHI `0xff63c000` + `0x2f4`, Bits [8:0] = N, `sys_pll = N * 12 MHz`.
- Stock big max = 2400 MHz (N=200). Gemessen stabil: **N=208 = 2496 MHz**
  (120 s Soak, 570 Laeufe, 0 Fehler). 2508/2520 grenzwertig, 2532 haengt.
- ACHTUNG: die Spannung ist am Regler-Anschlag (100 % duty = 1040 mV); oberhalb
  von 2400 MHz laeuft alles UNTERSPANNT. Das ist ein Experiment, kein Dauerbetrieb.

Ziel (reine Repo-Arbeit):
1. Erweitere die bestehende Overlay-Erzeugung - entweder `gen_lowfreq_dts.py`
   oder besser ein GETRENNTES neues Skript `gen_oc_dts.py` - so, dass ein
   OPP-Punkt **2496 MHz** mit passender `opp-microvolt` in den big-Cluster
   aufgenommen werden kann. Der Standardlauf MUSS die committete
   `remote-shell/ha/lowfreq.dts` unveraendert lassen.
2. Boot-Sicherheit: das Overlay muss beim ersten Reboot automatisch wieder
   verschwinden, wenn es nicht ausdruecklich "befoerdert" wird (Muster wie in
   `LOWFREQ-README.md` / `deploy_lowfreq_safe.sh`). Eine instabile Frequenz darf
   das Board nicht dauerhaft in eine Boot-Schleife bringen.
3. Host-Tests (`remote-shell/ha/test_gen_oc_dts.py`) fuer: 2496 wird erzeugt;
   Werte unter dem Regler-Boden werden abgelehnt; Standardlauf unveraendert.
4. `remote-shell/ha/OC-README.md`: Kurzdoku (Register, Messwerte, Risiko).

Abnahme: Standardlauf byte-identisch zu `lowfreq.dts`; neue Tests gruen.
NICHT anfassen: `PLL-OC-README.md`, `LOWFREQ-README.md`, `deploy_lowfreq*.sh`,
`revert_lowfreq.sh`, `pll_oc_probe.py`, `test_n.py`.
""",
    ),
    dict(
        title='ODROID: Watchdog-Luecke nach CPU-Hang (Doku + Haertung)',
        branch='docs/odroid-watchdog-gap',
        prompt="""Repo: christiansiebe/esp32-IR (Basis-Branch cline/power-history-diag).

Aufgabe: Die Watchdog-Luecke nach einem CPU-Hang dokumentieren und eine
repo-seitige Haertung vorschlagen.

Befund (2026-10-18, real beobachtet): Nach einem PLL-Hang bei 2532 MHz hat der
60-s-Hardware-Watchdog das Board **NICHT** rebootet. `journalctl --list-boots`
zeigt eine Luecke von ~66 min (Vor-Boot-Ende 02:15:01, naechster Boot 03:21:33).
Erwartet war ein Reboot nach 60 s (`RuntimeWatchdogUSec=1min`, `panic=10`). Das
Board lief anschliessend wieder normal (Stock-Takt).

Ziel (reine Repo-Arbeit, KEIN Board-Zugriff):
1. `remote-shell/ha/WATCHDOG-GAP.md`: den Befund festhalten; die beteiligten
   Mechanismen erklaeren (systemd `RuntimeWatchdogUSec`, Meson-Watchdog
   `/dev/watchdog0`, `watchdog-guard.timer`, Kernel `panic=10`) und begruendete
   Hypothesen, warum der HW-Watchdog nicht feuerte (z. B. externer
   Watchdog-Hold, `nowayout=0`, verlorener Kick-Task, PLL-Hang friert auch den
   Kick-Task ein). KEINE erfundenen Messwerte - klar trennen, was BELEGT ist und
   was HYPOTHESE.
2. Ein Haertungs-Vorschlag als Skript/Unit, das/der ein haengendes Board
   zuverlaessig rebootet (z. B. ein zweiter, unabhaengiger Watchdog-Pfad oder
   ein externer Heartbeat, den der Sandbox-Kanal ueberwacht). Vorteile und
   Restrisiken benennen.
3. Wenn ohne Board sinnvoll testbar: kleine Host-Tests fuer die
   Entscheidungslogik (reine Funktionen, keine Mocks).

Abnahme: Markdown + optionales Skript mit `sh -n` sauber.
NICHT anfassen: `PLL-OC-README.md` (nur referenzieren), bestehende
watchdog-guard-Dateien im Repo (nur referenzieren).
""",
    ),
]


def api(path, payload=None, method='GET'):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method)
    req.add_header('Authorization', 'Bearer ' + KEY)
    req.add_header('Content-Type', 'application/json')
    req.add_header('Accept', 'application/json')
    with urllib.request.urlopen(req, timeout=120) as r:
        raw = r.read().decode()
    return json.loads(raw) if raw.strip() else {}


def main():
    if not KEY:
        sys.exit('OPENHANDS_API_KEY fehlt')
    started = []
    for job in JOBS:
        body = dict(
            initial_message={'content': [{'type': 'text',
                                          'text': job['prompt'] + COMMON}]},
            selected_repository=REPO,
            selected_branch=BRANCH,
            parent_conversation_id=PARENT,
            title=job['title'],
        )
        try:
            res = api('/api/v1/app-conversations', body, 'POST')
        except Exception as e:                      # noqa: BLE001
            print('FEHLER beim Start von %r: %s' % (job['title'], e))
            continue
        sid = res.get('id')
        cid = res.get('app_conversation_id')
        print('gestartet: %-52s start_task=%s conv=%s' % (job['title'], sid, cid))
        started.append((job, sid, cid))

    print('\n--- auf READY warten ---')
    for job, sid, cid in started:
        for _ in range(40):
            if cid:
                break
            try:
                st = api('/api/v1/app-conversations/start-tasks?ids=' + str(sid))
            except Exception as e:                  # noqa: BLE001
                print('  poll fehler %s: %s' % (job['title'], e))
                time.sleep(5)
                continue
            items = st.get('items') if isinstance(st, dict) else st
            items = items or (st.get('start_tasks') if isinstance(st, dict) else None) or []
            if not items and isinstance(st, dict):
                items = [st]
            one = items[0] if items else {}
            cid = one.get('app_conversation_id')
            status = one.get('status')
            if cid:
                print('  %-52s READY conv=%s' % (job['title'], cid))
                break
            if status in ('FAILED', 'ERROR', 'CANCELLED'):
                print('  %-52s Startstatus=%s' % (job['title'], status))
                break
            time.sleep(6)
        else:
            print('  %-52s kein READY (start_task=%s)' % (job['title'], sid))
        if cid:
            print('     UI: %s/canvas/conversations/%s' % (BASE, cid))


if __name__ == '__main__':
    main()
