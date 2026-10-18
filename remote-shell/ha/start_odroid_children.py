#!/usr/bin/env python3
"""Startet die Odroid-Familie-Sub-Agenten fuer diesen Parent-Chat.

Parent = f85f7c473e7748259d2751fc1085b2b2 ("Odroid N2: VNC-Desktop & CPU-PLL-OC").

Arbeitsteilung (bewusst ueberschneidungsfrei):
  1  Odroid-Netz: AP-Uebernahme + Kamera  -> feat/odroid-lan-ap-camera
  2  Watchdog: externer Aktor (Design)    -> feat/odroid-watchdog-actor
  3  Gestapelte Odroid-PRs reviewen       -> docs/odroid-pr-review-5-8
  4  Firmware: record_count + web_guard   -> fix/record-count-web-guard

Board-Arbeit bleibt beim Parent: der remote-shell-Kanal ist eine GETEILTE
Verbindung. Kein Sub-Agent fasst das Board an.

Aufruf:  python3 start_odroid_children.py
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
PARENT = 'f85f7c473e7748259d2751fc1085b2b2'

COMMON = """
WICHTIG - Randbedingungen (verbindlich):
- NICHT auf `main` pushen. Arbeite auf dem unten genannten NEUEN Branch und
  oeffne einen Pull Request GEGEN `cline/power-history-diag`.
- Fasse das Odroid-Board NICHT an und benutze NICHT den remote-shell-Kanal
  (tools/remote-shell/*, cmd_send.sh, /dev/mem, systemctl am Board). Der Kanal
  ist eine geteilte Verbindung; parallele Nutzung fuehrt zu doppelt
  ausgefuehrten Befehlen und Board-Haengern. Reine Repo-/Host-Arbeit.
- Committe frueh und oft (kleine Commits) - die Sandbox kann jederzeit
  zurueckgesetzt werden. Beende mit kurzer Zusammenfassung (Status, Branch, PR).
- Halte `./test/run_host_tests.sh` gruen (fuege neue Tests dort ein, falls du
  welche anlegst).
- Fehlt ein Werkzeug/eine Abhaengigkeit: installieren oder Schritt mit Notiz
  ueberspringen, NICHT deswegen abbrechen.
"""

JOBS = [
    dict(
        title='ODROID: LAN/AP-Uebernahme + Kamera (Plan + Configs)',
        branch='feat/odroid-lan-ap-camera',
        prompt="""Repo: christiansiebe/esp32-IR (Basis: cline/power-history-diag).

Auftrag (Nutzer, 2026-10-18): Das LAN-Kabel steckt aktuell an einem Access
Point; der Odroid soll diese Aufgabe uebernehmen, und kuenftig soll eine
Kamera ueber LAN am Odroid haengen.

Vorhandener Recon (bereits erhoben, NICHT neu messen - kein Board-Zugriff!).
Der vollstaendige Recon liegt als `remote-shell/ha/ODROID-NETZ-RECON.md` auf
dem Branch `feat/dfs-clock-visibility` (auf deiner Basis `cline/power-history-diag`
noch nicht vorhanden - hole ihn bei Bedarf mit
`git show origin/feat/dfs-clock-visibility:remote-shell/ha/ODROID-NETZ-RECON.md`).
Kern:
- Ethernet `end0` ist NO-CARRIER (Kabel steckt am AP, nicht am Odroid).
- WLAN-STA `wlxdc627949ba79` (USB rtw88_8822bu) ist UP, 192.168.178.71,
  SSID "Wohnzimmer 5 Ghz", Default-Route via 192.168.178.1 (FRITZ!Box).
- `hostapd`/`dnsmasq` nicht installiert, aber installierbar (bookworm main).
- `/dev/video0` = nur der Amlogic-Video-Decoder, KEINE Kamera.

Aufgabe (reine Repo-Arbeit, KEIN Board-Zugriff):
1) Neue Doku `remote-shell/ha/ODROID-LAN-AP-KAMERA.md` mit einem konkreten,
   umkehrbaren Umsetzungsplan:
   - Zielarchitektur: `end0` = Uplink ins Heimnetz, WLAN = AP fuer die alten
     AP-Clients. Reihenfolge zwingend: erst `end0`-Uplink verifizieren, DANN
     WLAN auf AP umstellen (sonst Lockout - nur EIN Funkmodul!).
   - Hostapd-/dnsmasq-Konfigurationsvorlagen (SSID/Kanal/Passwort als
     Platzhalter, NICHT raten), NAT/Bridge-Regeln auf `end0`,
     `net.ipv4.ip_forward=1` ist schon gesetzt.
   - Kamera: beide Wege (IP-Kamera RTSP/ONVIF vs. USB-UVC/V4L2) mit
     Home-Assistant-Einbindung (ONVIF-/Generic-Camera- bzw. UVC-Integration);
     Entscheidungskriterien, weil Kamera-Typ noch offen ist.
   - Ein Rollback-/Notfall-Abschnitt (wie komme ich zurueck, wenn das WLAN
     zum AP wird und der Kanal stirbt).
   - Ein Abschnitt "Offene Nutzer-Entscheidungen" (welcher AP? ersetzen oder
     ergaenzen? IP- vs. USB-Kamera? AP vs. Router?).
2) Optional: kleine, host-testbare Hilfsskripte (z. B. ein Generator fuer die
   hostapd/dnsmasq-Dateien mit --dry-run), wenn sie ohne Board testbar sind.

NICHT anfassen: ODROID-NETZ-RECON.md (nur referenzieren), andere Odroid-Dateien.
""",
    ),
    dict(
        title='ODROID: Watchdog externer Aktor (Design + Integration)',
        branch='feat/odroid-watchdog-actor',
        prompt="""Repo: christiansiebe/esp32-IR (Basis: cline/power-history-diag).

Kontext: `remote-shell/ha/WATCHDOG-GAP.md` dokumentiert, dass der 60-s-
Hardware-Watchdog nach einem CPU-PLL-Hang NICHT zuverlaessig rebootet. Der
Parent hat am Board verifiziert (Abschnitt 8 der Doku): H2 (Device busy) und
H1 (Guard-Timer entwaffnet) sind WIDERLEGT, `bootstatus` ist auf Amlogic immer
0 (unbrauchbar) -> H3 (Reset kam nicht durch) ist die beste verbleibende
Erklaerung. Der bereits gemergte `watchdog_monitor.py` implementiert den
Off-Board-Beobachter mit reiner `decide()`-Logik und einem `--reboot-cmd`-Aktor.

Aufgabe (reine Repo-Arbeit, KEIN Board-Zugriff):
1) Neues Doku `remote-shell/ha/WATCHDOG-ACTOR.md`: Wie wird der OFF-BOARD-Aktor
   real angeschlossen? Vergleiche mindestens: (a) schaltbare Steckdose
   (Tasmota/ESPHome/HA-Switch) per HTTP/MQTT, (b) USB-Relais/GPIO an einem
   zweiten Rechner (z. B. der Odroid-MCU/ESP32), (c) Reset-Leitung. Fuer jede
   Option: Voraussetzungen, Sicherheitsrisiko (Fehlausloesung = Stromausfall!),
   und ein sicheres Cooldown/Quorum-Konzept.
2) Ein Aktor-Adapter-Skript `remote-shell/ha/watchdog_actor.py` mit reiner,
   host-testbarer Logik (z. B. Bau der HTTP-/MQTT-Anfrage, Rate-Limit/Cooldown,
   Trockenlauf) - NICHT den echten Aktor ausloesen. Fuege
   `remote-shell/ha/test_watchdog_actor.py` hinzu (keine Mocks fuer die reine
   Logik) und haenge es an `test/run_host_tests.sh` an.
3) Kurzer Abschnitt: wie man den Monitor in `watchdog-heartbeat.timer`-
   Umgebung produktiv startet (systemd-Unit-Vorschlag fuer den OFF-Board-Host).

NICHT anfassen: `watchdog_monitor.py`, `test_watchdog_monitor.py`,
`watchdog-heartbeat.*`, `WATCHDOG-GAP.md` (nur referenzieren).
""",
    ),
    dict(
        title='ODROID: gestapelte PRs #5-#8, #14 reviewen',
        branch='docs/odroid-pr-review-5-8',
        prompt="""Repo: christiansiebe/esp32-IR.

Aufgabe: Pruefe die offenen Odroid-PRs gegen den Basis-Branch
`cline/power-history-diag` und fasse je PR zusammen:

  #5  feat(odroid): one-boot test overlay + stock restore scripts
  #6  feat(odroid): per-rate voltage curve for the lowfreq OPP overlay
  #7  docs(odroid): research notes on the pwm-regulator and Amlogic DVFS
  #8  docs(odroid): review of the open PRs #5, #6, #7
  #14 feat(odroid): ramoops/pstore - Kernel-Panics ueberleben den Warm-Reset

Fuer jeden PR:
- Lies den Diff (`git diff origin/cline/power-history-diag...origin/<head>`).
- Pruefe: erfuellt er seinen Zweck? Grobe Fehler, Sicherheitsluecken (v. a.
  Boot-Loop-Risiko bei CPU-Spannungs-Aenderungen!), widerspruechliche Aussagen?
  Shell-Syntax (`sh -n`) bei .sh-Dateien, Host-Tests ausfuehren falls vorhanden.
- Pruefe, ob Inhalte bereits auf dem Basis-Branch liegen (dann ist der PR
  ueberholt). Pruefe auch, ob #8 (Review) durch die spaeteren Merges (#18-#20,
  #35) veraltet ist.
- Empfehlung je PR: MERGE / CLOSE (mit Grund) / UPDATE (was fehlt).

Schreibe das Ergebnis als `remote-shell/ha/PR-REVIEW-ODROID.md` auf einem neuen
Branch `docs/odroid-pr-review-5-8` und oeffne einen PR gegen
cline/power-history-diag. Aendere die PRs selbst NICHT und merge nichts.
""",
    ),
    dict(
        title='Firmware: record_count + web_guard (Host-Analyse)',
        branch='fix/record-count-web-guard',
        prompt="""Repo: christiansiebe/esp32-IR (ESP-IDF Firmware; Basis cline/power-history-diag).

Zwei offene Firmware-TODOs aus `TODO.md` host-seitig bearbeiten (KEIN Board):

1) `record_count` nach dem Boot nicht wiederhergestellt (TODO ~232):
   Der bekannte Ausloeser (`1612975`, ungueltiger Fresh-Header `magic=0`) ist
   behoben; der Speicherpfad hat jetzt einen Host-Test
   (`test/test_reboot_persistence.c`). Zu tun host-seitig: Header-/Migrations-
   Pfade (gueltiger Bestand darf NICHT als leer gelten; alte `format_version`
   darf Datensaetze nicht verwerfen) zusaetzlich als Host-Tests abdecken und
   die Logzeile `Persistent history ready: N of ...` gegen `record_count`
   pruefbar machen. Am Geraet verifiziert der Parent - du nur host-seitig.

2) `web_guard` startet den HTTP-Server im Leerlauf neu (TODO ~81):
   `restarts` steigt ~1,2/Minute ohne externe Zugriffe, Loopback-Probe
   funktioniert trotzdem. Vermutung: die 5-s-Loopback-Probe (`esp_http_client`
   auf 127.0.0.1:80) schlaegt gelegentlich fehl oder kollidiert mit
   `max_open_sockets` -> Guard wertet das als "Server tot". Analysiere den
   Code (`main/web_guard*`, HTTPD-Setup) und behebe die Ursache: Probe-Intervall/
   Timeout, `threshold_ms`/`restart_timeout_ms`, oder Probe nicht gegen sich
   selbst. Wenn moeglich als Host-Test der Entscheidungslogik absichern.

Abnahme: `./test/run_host_tests.sh` gruen (neue Tests dort eintragen).
NICHT anfassen: Odroid-Dateien (`remote-shell/`).
""",
    ),
]


def api(path, payload=None, method='GET'):
    req = urllib.request.Request(
        BASE + path,
        data=json.dumps(payload).encode() if payload is not None else None,
        method=method)
    req.add_header('Authorization', 'Bearer ' + KEY)
    req.add_header('Content-Type', 'application/json')
    with urllib.request.urlopen(req, timeout=90) as r:
        return json.load(r)


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
            title=job['title'],
            parent_conversation_id=PARENT,
        )
        try:
            res = api('/api/v1/app-conversations', body, 'POST')
        except Exception as e:                      # noqa: BLE001
            print('FEHLER beim Start von %r: %s' % (job['title'], e))
            continue
        sid = res.get('id')
        cid = res.get('app_conversation_id')
        print('gestartet: %-46s start_task=%s conv=%s' % (job['title'], sid, cid))
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
            items = items or []
            if not items and isinstance(st, dict):
                items = [st]
            one = items[0] if items else {}
            cid = one.get('app_conversation_id')
            if cid:
                print('  %-46s READY conv=%s' % (job['title'], cid))
                break
            if one.get('status') in ('FAILED', 'ERROR', 'CANCELLED'):
                print('  %-46s Startstatus=%s' % (job['title'], one.get('status')))
                break
            time.sleep(6)
        else:
            print('  %-46s kein READY (start_task=%s)' % (job['title'], sid))
        if cid:
            print('     UI: %s/canvas/conversations/%s' % (BASE, cid))


if __name__ == '__main__':
    main()
