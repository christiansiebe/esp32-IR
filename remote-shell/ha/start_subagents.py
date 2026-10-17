#!/usr/bin/env python3
"""Startet die delegierten Sub-Agenten (eigene Cloud-Conversations).

Arbeitsteilung (bewusst ueberschneidungsfrei):
  A  Generator + Host-Tests      -> feat/per-rate-opp-voltage
  B  Sicherer Overlay + SD-Rettung -> feat/safe-overlay-rollback
  C  Recherche/Doku (neu)         -> docs/pwm-regulator-notes

Board-Arbeit bleibt beim Haupt-Chat: der Kanal ist eine geteilte Verbindung,
und cmd_agent.py hat keine Sperre - zwei Agenten wuerden denselben Befehl
doppelt ausfuehren. Deshalb steht in jedem Brief: Board NICHT anfassen.

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

COMMON = """
WICHTIG - Randbedingungen (verbindlich):
- NICHT auf `main` pushen. Arbeite auf dem unten genannten NEUEN Branch und
  oeffne einen Pull Request GEGEN `cline/power-history-diag`.
- Fasse das Odroid-Board NICHT an und benutze NICHT den remote-shell-Kanal
  (tools/remote-shell/*, cmd_send.sh, /dev/mem, systemctl am Board). Der Kanal
  ist eine geteilte Verbindung; parallele Nutzung fuehrt zu doppelt ausgefuehrten
  Befehlen und Board-Haengern. Diese Aufgabe ist reine Repo-/Host-Arbeit.
- Halte dich von Dateien fern, die andere Agenten besitzen (steht je Aufgabe).
- Committe frueh und oft (kleine Commits), damit bei einem Sandbox-Reset nichts
  verloren geht. Der Chat kann nach ~20 min Inaktivitaet zurueckgesetzt werden.
- Absturzsicherheit: Wenn ein Werkzeug/ eine Abhaengigkeit fehlt, installiere es
  oder ueberspringe den Schritt mit Notiz - brich die Aufgabe NICHT deswegen ab.
  Beende mit einer kurzen Zusammenfassung (was fertig, was offen, Branch, PR-URL).
"""

JOBS = [
    dict(
        title='ODROID: per-rate OPP voltage + guards',
        branch='feat/per-rate-opp-voltage',
        prompt="""Repo: christiansiebe/esp32-IR (Branch cline/power-history-diag als Basis).

Aufgabe: Erweitere `remote-shell/ha/gen_lowfreq_dts.py` um eine SPANNUNGS-KURVE
pro Takt (nicht mehr ein Wert fuer alle Tief-Takte eines Clusters).

Kontext: Der Odroid N2+ CPU-Regler ist ein PWM-Regler. Das Overlay fuegt
OPP-Punkte unter 1 GHz hinzu; `gen_lowfreq_dts.py` schreibt `opp-microvolt` je
Punkt. Heute gilt `--big-mv`/`--little-mv` fuer ALLE Raten eines Clusters.

Ziel:
- Pro Rate eine eigene Spannung setzen koennen (z.B. `--big-table
  "24000=700,50000=690,..."` oder eine JSON-Datei - waehle ein sauberes,
  dokumentiertes Design).
- Harte Schutzgrenzen: Werte unter der Regler-Untergrenze 680000 uV ABLEHNEN;
  Werte unter 695000 uV zulassen, aber deutlich warnen (Reserve); unbekannte
  Raten und mehr als 14 Raten ablehnen.
- Standardlauf (ohne Argumente) muss byte-identisch zur committeten
  `remote-shell/ha/lowfreq.dts` bleiben (Test prueft das bereits).
- Erweitere `remote-shell/ha/test_gen_lowfreq_dts.py` um Host-Tests fuer die
  neuen Grenzen (unter Boden -> Fehler, Warnung bei <695, per-Rate-Kurve greift).

Abnahme: `python3 remote-shell/ha/test_gen_lowfreq_dts.py` laeuft gruen.

NICHT anfassen: `remote-shell/ha/LOWFREQ-README.md`, deploy/revert-Skripte.
""",
    ),
    dict(
        title='ODROID: safe overlay rollback + stock restore',
        branch='feat/safe-overlay-rollback',
        prompt="""Repo: christiansiebe/esp32-IR (Branch cline/power-history-diag als Basis).

Aufgabe: Zwei NEUE Shell-Skripte in `remote-shell/ha/`:

1) `deploy_lowfreq_safe.sh` - installiert das CPU-Overlay als TEST-Overlay, das
   beim NAECHSTEN Reboot automatisch wieder verschwindet, solange es nicht
   ausdruecklich "befoerdert" (dauerhaft) wird. Vorschlag: `user_overlays=` bleibt
   leer; eine oneshot-systemd-Unit traegt beim Boot das Overlay nur ein, wenn eine
   Marker-Datei existiert, und LOESCHT den Marker nach dem ersten Boot - der erste
   Reboot kehrt also zu Werkswerten zurueck. Idempotent und umkehrbar.
2) `restore-stock.sh` - stellt den Werkszustand von der SD-Karte her:
   `user_overlays`-Zeile entfernen, `/boot/overlay-user/lowfreq.dtbo` loeschen,
   `cpufreq.default_governor=performance` aus `extraargs` entfernen,
   `cpu-freq-guard.service` + `cpu-hotplug.service` deaktivieren+loeschen,
   `MIN_SPEED=1000000` und `GOVERNOR=schedutil` in `/etc/default/cpufrequtils`,
   alle CPUs wieder online. Klare Zusammenfassung ausgeben, Reboot verlangen.

Kontext Boot-Schutz heute: `cpufreq.default_governor=performance` +
`cpu-freq-guard.service` klemmt `scaling_min_freq` auf 1.0 GHz.

Abnahme: `sh -n` (Syntax) sauber; ein Dry-Run/`--help`-Pfad, der ohne Board
sinnvoll abbricht. Kein Test benoetigt das echte Board.

NICHT anfassen: `gen_lowfreq_dts.py`, `test_gen_lowfreq_dts.py`,
`LOWFREQ-README.md`.
""",
    ),
    dict(
        title='ODROID: pwm-regulator research notes',
        branch='docs/pwm-regulator-notes',
        prompt="""Repo: christiansiebe/esp32-IR (Branch cline/power-history-diag als Basis).

Aufgabe: EINE NEUE Datei `remote-shell/ha/PWM-REGULATOR-NOTES.md` schreiben.

Kontext: Odroid N2+ (Amlogic S922X, meson-g12b). Die CPU-Kernspannung erzeugt ein
PWM-Regler: DT-Nodes `regulator-vddcpu-a` (big, policy2) und
`regulator-vddcpu-b` (little, policy0), compatible "pwm-regulator". Heute im
Modus "Continuous Voltage": `regulator-min-microvolt=680000`,
`regulator-max-microvolt=1040000`, `pwm-dutycycle-range=<100 0>`.

Recherchiere und dokumentiere, MIT Quellen-URLs:
- Das Linux `pwm-regulator`-Binding: beide Modi (continuous vs. voltage-table),
  die Eigenschaften (`voltage-table`, `pwm-dutycycle-unit`, `pwm-dutycycle-range`)
  und wie der Treiber in jedem Modus eine Wunschspannung auf ein Tastverhaeltnis
  abbildet.
- Ob eine `voltage-table` das Kalibrier-Problem beseitigt, dass eine Aenderung von
  `regulator-min-microvolt` jeden anderen Sollwert verschiebt.
- Amlogic-meson-DVFS: wer auf dem S922X Spannung/Takt wirklich steuert (Cortex-M
  Co-Prozessor / BL31-Firmware) und was das fuer DT-basiertes Undervolting heisst.
- Vorhandene Hardkernel/Armbian/Community-Doku zur ODROID-N2-CPU-Spannung.

Erfinde KEINE Messwerte. Externe Zahlen klar als extern kennzeichnen.

NICHT anfassen: alle vorhandenen Dateien. Nur die neue Markdown-Datei anlegen.
""",
    ),
]


def api(path, payload=None, method='GET'):
    url = BASE + path
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
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
        )
        try:
            res = api('/api/v1/app-conversations', body, 'POST')
        except Exception as e:                      # noqa: BLE001
            print('FEHLER beim Start von %r: %s' % (job['title'], e))
            continue
        sid = res.get('id')
        cid = res.get('app_conversation_id')
        print('gestartet: %-42s start_task=%s conv=%s' % (job['title'], sid, cid))
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
                print('  %-42s READY conv=%s' % (job['title'], cid))
                break
            if status in ('FAILED', 'ERROR', 'CANCELLED'):
                print('  %-42s Startstatus=%s' % (job['title'], status))
                break
            time.sleep(6)
        else:
            print('  %-42s kein READY nach Wartezeit (start_task=%s)'
                  % (job['title'], sid))
        if cid:
            print('     UI: %s/canvas/conversations/%s' % (BASE, cid))


if __name__ == '__main__':
    main()
