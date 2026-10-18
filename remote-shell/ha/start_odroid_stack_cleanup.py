#!/usr/bin/env python3
"""Startet den Odroid-PR-Stack-Cleanup-Sub-Agenten fuer Parent 30cfd896.

Parent = 30cfd896aac440a4a09ba70ec6199f08 ("Odroid Entwicklung (neu)").
Grundlage: PR #38 (PR-REVIEW-ODROID.md, gemerged f1cd7c7) empfiehlt je PR
UPDATE/CLOSE. Arbeitsteilung: der Sub-Agent baut die Fixes auf NEUEN Branches
und oeffnet PRs gegen cline/power-history-diag; er fasst das Board NICHT an.

Aufruf:  python3 start_odroid_stack_cleanup.py
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
PARENT = '30cfd896aac440a4a09ba70ec6199f08'

COMMON = """
WICHTIG - Randbedingungen (verbindlich):
- NICHT auf `main` pushen. Arbeite auf dem unten genannten NEUEN Branch und
  oeffne Pull Requests GEGEN `cline/power-history-diag`.
- Fasse das Odroid-Board NICHT an und benutze NICHT den remote-shell-Kanal
  (tools/remote-shell/*, cmd_send.sh, /dev/mem, systemctl am Board). Der Kanal
  ist eine GETEILTE Verbindung. Reine Repo-/Host-Arbeit.
- Committe frueh und oft (kleine Commits) - die Sandbox kann jederzeit
  zurueckgesetzt werden. Beende mit kurzer Zusammenfassung (Status, Branch, PR).
- Halte `./test/run_host_tests.sh` gruen (fuege neue Tests dort ein, falls du
  welche anlegst).
- Fehlt ein Werkzeug/eine Abhaengigkeit: installieren oder Schritt mit Notiz
  ueberspringen, NICHT deswegen abbrechen.
"""

JOBS = [
    dict(
        title='ODROID: PR-Stack #5/#6/#7/#8/#14 aufraeumen',
        branch='fix/odroid-pr-stack',
        prompt="""Repo: christiansiebe/esp32-IR (Basis: cline/power-history-diag).

Kontext: `remote-shell/ha/PR-REVIEW-ODROID.md` (auf cline/power-history-diag,
gemerged via PR #38) bewertet die gestapelten Odroid-PRs. Setze die Empfehlungen
um - je PR auf einem EIGENEN neuen Branch, jeweils PR gegen
cline/power-history-diag. Die Original-PRs nicht direkt anfassen, ausser es ist
unten explizit gesagt.

Vorgehen je PR (Reihenfolge egal, aber sauber getrennt):
  #8  docs/pr-review-5-6-7 -> CLOSE-Empfehlung: durch #38 (PR-REVIEW-ODROID.md)
      ueberholt. Pruefe das und schliesse den PR mit einem kurzen Kommentar
      ("superseded by #38"), falls er wirklich ueberholt ist.
  #7  docs/pwm-regulator-notes -> UPDATE (Genauigkeit). Uebernimm die Doku auf
      einen neuen Branch, korrigiere die beanstandeten Stellen und oeffne PR.
  #14 docs/uv-software-limit-report -> UPDATE: Rebase auf cline/power-history-diag
      und Scope teilen (66 Dateien sind zu viel fuer einen PR) - z. B. nur den
      Kernbericht + zugehoerige Tests, Rest als eigenen PR. Staleness pruefen.
  #6  feat/lowfreq-voltage-curve -> UPDATE (blockierend): hat laut Review einen
      Merge-Konflikt in `remote-shell/ha/gen_lowfreq_dts.py` und bricht
      `gen_oc_dts.py`. Rebase, Konflikt loesen, Regression reparieren, Host-Tests
      gruen, PR oeffnen.
  #5  feat/lowfreq-test-overlay-safe-restore -> UPDATE (kleine Fixes): `sh -n` ok,
      Test 21/28 (umgebungsabhaengig). Kleine Fixes anwenden und PR oeffnen.

Lies zuerst `remote-shell/ha/PR-REVIEW-ODROID.md` vollstaendig - dort stehen die
konkreten Befunde und Zeilennummern. Verifiziere jeden Befund selbst, bevor du
ihn umsetzt (der Review ist Stand bcb9938).

Abnahme: je PR `./test/run_host_tests.sh` gruen (bzw. `sh -n` fuer reine
Shell-PRs). Kein Merge, keine Board-Arbeit.
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
