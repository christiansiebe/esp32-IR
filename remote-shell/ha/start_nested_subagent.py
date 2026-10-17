#!/usr/bin/env python3
"""Startet EINEN delegierten Sub-Agenten, verschachtelt unter diesem Chat.

Nesting geht nur beim ERZEUGEN: `parent_conversation_id` steht nur im
POST-Schema (AppConversationStartRequest-Input); PATCH ignoriert das Feld
still (probiert, HTTP 200, Wert bleibt None). Es gibt keinen eigenen
sub/parent-Endpunkt. Ein bereits laufender/fertiger Chat laesst sich also NICHT
nachtraeglich unterordnen - nur neu erzeugt.

Aufruf:
    python3 start_nested_subagent.py <parent_conv_id> "<titel>" "<prompt>"
    python3 start_nested_subagent.py <parent_conv_id> --pr-review 5,6,7
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
Randbedingungen: NICHT auf main pushen; PR gegen cline/power-history-diag.
Fasse das Odroid-Board NICHT an und benutze NICHT den remote-shell-Kanal
(geteilte Verbindung -> doppelte Befehlsausfuehrung). Reine Repo-/Host-Arbeit.
Committe frueh und oft. Beende mit kurzer Zusammenfassung (Status, Branch, PR).
"""

PR_REVIEW = """Repo: christiansiebe/esp32-IR.

Aufgabe: Pruefe die drei offenen PRs #5, #6, #7 gegen den Basis-Branch
`cline/power-history-diag` und fasse je PR zusammen:

  #5 feat(odroid): one-boot test overlay + stock restore scripts
  #6 feat(odroid): per-rate voltage curve for the lowfreq OPP overlay
  #7 docs(odroid): research notes on the pwm-regulator and Amlogic DVFS

Fuer jeden PR:
- Lies den Diff (git diff origin/cline/power-history-diag...origin/<head>).
- Pruefe: erfuellt er seinen Zweck? Grobe Fehler, Sicherheitsluecken,
  widerspruechliche Aussagen? Shell-Syntax (sh -n) bei .sh-Dateien.
- Melde konkrete Probleme mit Datei/Zeile, sonst "merge-faehig".

Besonders #6 und #5 beruehren die CPU-Spannung des Odroid (Boot-Loop-Risiko).
Achte darauf, dass Schutzgrenzen (keine Werte unter der Regler-Untergrenze,
Warnung unter Reserve) wirklich im Code stehen, nicht nur in Kommentaren.

Schreibe das Ergebnis als Markdown-Datei `remote-shell/ha/PR-REVIEW.md` auf
einem neuen Branch `docs/pr-review-5-6-7` und oeffne einen PR gegen
cline/power-history-diag. Aendere die PRs selbst NICHT.
"""


def api(path, payload=None, method='GET'):
    req = urllib.request.Request(BASE + path,
                                 data=json.dumps(payload).encode() if payload else None,
                                 method=method)
    req.add_header('Authorization', 'Bearer ' + KEY)
    req.add_header('Content-Type', 'application/json')
    with urllib.request.urlopen(req, timeout=90) as r:
        return json.load(r)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    parent = sys.argv[1]
    if sys.argv[2] == '--pr-review':
        title = 'PR-Review: #5 #6 #7 (odroid overlay/voltage/research)'
        prompt = PR_REVIEW
    else:
        title = sys.argv[2]
        prompt = sys.argv[3] if len(sys.argv) > 3 else ''
    body = dict(
        initial_message={'content': [{'type': 'text', 'text': prompt + COMMON}]},
        selected_repository=REPO,
        selected_branch=BRANCH,
        title=title,
        parent_conversation_id=parent,
    )
    res = api('/api/v1/app-conversations', body, 'POST')
    sid = res.get('id')
    cid = res.get('app_conversation_id')
    print('gestartet unter parent=%s: start_task=%s conv=%s' % (parent, sid, cid))
    for _ in range(40):
        if cid:
            break
        st = api('/api/v1/app-conversations/start-tasks?ids=' + str(sid))
        items = st if isinstance(st, list) else (st.get('items') or [st])
        one = items[0] if items else {}
        cid = one.get('app_conversation_id')
        if cid:
            print('READY conv=%s' % cid)
            break
        if one.get('status') in ('FAILED', 'ERROR', 'CANCELLED'):
            print('Startstatus=%s' % one.get('status'))
            break
        time.sleep(6)
    if cid:
        print('UI: %s/canvas/conversations/%s' % (BASE, cid))


if __name__ == '__main__':
    main()
