#!/usr/bin/env python3
"""Legt das native WP-Dashboard ('wp-native') in Home Assistant an.

Rein native Karten (gauge, statistic, entities, history-graph, statistics-graph,
heading) - kein Grafana, kein ApexCharts, keine Custom Card. Bestehende
Dashboards bleiben unberuehrt; die Anlage laeuft ueber die offizielle
WebSocket-API (lovelace/dashboards/create + lovelace/config/save), daher KEIN
HA-Neustart noetig.

Aufruf (im HA-Container oder mit Python + websockets auf dem Odroid):

    python3 deploy_wp_native_dashboard.py

Liest die Views aus ``wp_native_dashboard.json`` (gleiches Verzeichnis).
Idempotent: existiert 'wp-native' schon, wird nur die Config gespeichert.
Erstellt vorher ein Backup von ``.storage/lovelace.wp_native``.
"""
import asyncio
import json
import os
import shutil
import time
import urllib.parse
import urllib.request

import websockets

HA = os.environ.get("HA_URL", "http://127.0.0.1:8123")
WS = HA.replace("http://", "ws://").replace("https://", "wss://") + "/api/websocket"
ST = os.environ.get("HA_CONFIG", "/usr/share/hassio/homeassistant")
STORAGE = os.path.join(ST, ".storage")
URL_PATH = "wp-native"
TITLE = "WP Dashboard"
HERE = os.path.dirname(os.path.abspath(__file__))


def token():
    d = json.load(open(os.path.join(STORAGE, "auth")))
    rt = next(t for t in d["data"]["refresh_tokens"]
              if t.get("token_type") == "normal")
    form = {"grant_type": "refresh_token", "refresh_token": rt["token"]}
    if rt.get("client_id"):
        form["client_id"] = rt["client_id"]
    req = urllib.request.Request(
        HA + "/auth/token", data=urllib.parse.urlencode(form).encode(),
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    return json.loads(urllib.request.urlopen(req, timeout=10).read())["access_token"]


async def call(ws, mid, payload):
    await ws.send(json.dumps(dict(id=mid, **payload)))
    while True:
        r = json.loads(await ws.recv())
        if r.get("id") == mid:
            return r


async def main():
    views = json.load(open(os.path.join(HERE, "wp_native_dashboard.json")))["views"]
    p = os.path.join(STORAGE, "lovelace.wp_native")
    if os.path.exists(p):
        b = "%s.bak-%s" % (p, time.strftime("%Y%m%d-%H%M%S"))
        shutil.copy2(p, b)
        print("backup:", b)

    tok = token()
    async with websockets.connect(WS, max_size=None) as ws:
        await ws.recv()
        await ws.send(json.dumps({"type": "auth", "access_token": tok}))
        await ws.recv()
        r = await call(ws, 1, {"type": "lovelace/dashboards/list"})
        have = {d.get("url_path") for d in r.get("result", [])}
        if URL_PATH not in have:
            r = await call(ws, 2, {"type": "lovelace/dashboards/create",
                "url_path": URL_PATH, "title": TITLE, "icon": "mdi:heat-pump",
                "show_in_sidebar": True, "require_admin": False})
            print("dashboards/create:", r.get("success"))
        r = await call(ws, 3, {"type": "lovelace/config/save",
            "url_path": URL_PATH, "config": {"views": views}})
        print("config/save:", r.get("success"))
    print("FERTIG - Dashboard unter /%s erreichbar" % URL_PATH)


if __name__ == "__main__":
    asyncio.get_event_loop().run_until_complete(main())
