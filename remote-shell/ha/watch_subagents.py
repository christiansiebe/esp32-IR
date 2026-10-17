#!/usr/bin/env python3
"""Waechter fuer die delegierten Sub-Agenten.

Beobachtet die gestarteten Conversations und meldet, wenn eine stehenbleibt,
fehlschlaegt oder fertig ist. Startet NICHTS neu und fasst nichts am Board an -
er gibt nur einen klaren Statusbericht aus, damit der Haupt-Chat reagieren kann.

Aufruf:
    python3 watch_subagents.py <conv_id> [<conv_id> ...]
    python3 watch_subagents.py --all-running
"""
import json
import os
import sys
import time
import urllib.request

BASE = 'https://app.all-hands.dev'
KEY = os.environ.get('OPENHANDS_API_KEY') or os.environ.get('OPENHANDS_CLOUD_API_KEY')


def api(path):
    req = urllib.request.Request(BASE + path)
    req.add_header('Authorization', 'Bearer ' + KEY)
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def conv(cid):
    d = api('/api/v1/app-conversations?ids=' + cid)
    items = d.get('items') if isinstance(d, dict) else d
    return (items or [{}])[0]


def last_events(cid, n=3):
    try:
        d = api('/api/v1/conversation/%s/events/search?limit=%d' % (cid, n))
        items = d.get('items') or []
    except Exception as e:                          # noqa: BLE001
        return ['(events nicht lesbar: %s)' % e]
    out = []
    for e in items:
        kind = e.get('kind', '?')
        src = e.get('source', '?')
        ts = e.get('timestamp', '')
        out.append('%s %s %s' % (ts, src, kind))
    return out


def main():
    if not KEY:
        sys.exit('OPENHANDS_API_KEY fehlt')
    args = sys.argv[1:]
    if args == ['--all-running']:
        d = api('/api/v1/app-conversations/search?limit=30')
        items = d.get('items', [])
        ids = [i['id'] for i in items if i.get('execution_status') == 'running']
    else:
        ids = args
    if not ids:
        print('keine Conversations angegeben')
        return
    for cid in ids:
        c = conv(cid)
        print('%-36s sandbox=%-10s exec=%-10s %s' % (
            cid, c.get('sandbox_status'), c.get('execution_status'),
            c.get('title', '')))
        for line in last_events(cid):
            print('    %s' % line)
        print()


if __name__ == '__main__':
    main()
