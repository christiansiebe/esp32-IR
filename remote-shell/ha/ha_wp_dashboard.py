#!/usr/bin/env python3
"""Deploy the heat-pump zoom cards and their Lovelace resource.

Copies ``ha_wp_zoom_chart.js`` into HA's ``config/www/``, registers it as a
Lovelace module resource (idempotent) and adds the three zoom cards
(temperature, energy, power/COP) to the ``waermepumpe-dashboard`` if missing.

Runs **inside** the Home Assistant container so it can read ``.storage`` and
mint an admin JWT:

  docker exec homeassistant python3 /config/ha_wp_dashboard.py

The JS source is expected at ``/config/wp-zoom-chart.js`` (copy it there first).
"""
import asyncio
import json
import os
import shutil
import time

import jwt
import websockets

AUTH = "/config/.storage/auth"
DASH = "/config/.storage/lovelace.waermepumpe_dashboard"
RES_URL = "/local/wp-zoom-chart.js"

d = json.load(open(AUTH))
rt = [t for t in d["data"]["refresh_tokens"]
      if t.get("token_type") == "long_lived_access_token"][0]
now = int(time.time())
access = jwt.encode({"iss": rt["id"], "iat": now, "exp": now + 900},
                    rt["jwt_key"], algorithm="HS256")
if isinstance(access, bytes):
    access = access.decode()

URL = "ws://127.0.0.1:8123/api/websocket"

# --- Chart-Entitaeten ---
TEMPS = [
    {"entity": "sensor.mx_400_supply_temperature", "name": "Vorlauf", "kind": "mean"},
    {"entity": "sensor.mx_400_return_temperature", "name": "Rücklauf", "kind": "mean"},
    {"entity": "sensor.warmwasser_actual_temperature", "name": "Warmwasser", "kind": "mean"},
    {"entity": "sensor.mx_400_outdoor_temperature", "name": "Außen", "kind": "mean"},
]
ENERGY = [
    {"entity": "sensor.warmeerzeuger_2_total_energy_produced", "name": "Wärme erzeugt", "kind": "sum", "unit": "kWh"},
    {"entity": "sensor.warmeerzeuger_2_total_energy_compressor", "name": "Strom verbraucht", "kind": "sum", "unit": "kWh"},
]
LEISTUNG = [
    {"entity": "sensor.mx_400_compressor_electrical_power", "name": "Elektr. Leistung", "kind": "mean", "unit": "W"},
    {"entity": "sensor.wp_cop_1h", "name": "COP (1 h)", "kind": "mean", "unit": "COP", "opposite": True},
]


def zoom_card(title, entities, default="24h", height=260):
    return {"type": "custom:wp-zoom-chart", "title": title,
            "entities": entities, "default_range": default, "height": height}


def find_view(cfg):
    return cfg["views"][0]


async def main():
    # 1) JS kopieren
    src = "/config/wp-zoom-chart.js"
    dst = "/config/www/wp-zoom-chart.js"
    if os.path.exists(src):
        shutil.copy2(src, dst)
        print("JS kopiert:", dst, os.path.getsize(dst), "Bytes")
    else:
        print("WARNUNG: /config/wp-zoom-chart.js fehlt")

    async with websockets.connect(URL, max_size=32 * 1024 * 1024) as ws:
        await ws.recv()
        await ws.send(json.dumps({"type": "auth", "access_token": access}))
        r = json.loads(await ws.recv())
        if r["type"] != "auth_ok":
            print("AUTH FEHLER:", r)
            return

        async def call(mid, payload):
            payload["id"] = mid
            await ws.send(json.dumps(payload))
            while True:
                msg = json.loads(await ws.recv())
                if msg.get("id") == mid and msg.get("type") == "result":
                    return msg

        # 2) Ressource registrieren
        r = await call(1, {"type": "lovelace/resources"})
        existing = {x.get("url", "").split("?")[0] for x in (r.get("result") or [])}
        if RES_URL not in existing:
            r = await call(2, {"type": "lovelace/resources/create",
                               "res_type": "module", "url": RES_URL})
            print("Ressource erstellt:", r.get("success"), r.get("error") or "")
        else:
            print("Ressource existiert bereits")

        # 3) Dashboard laden
        r = await call(3, {"type": "lovelace/config",
                           "url_path": "waermepumpe-dashboard"})
        cfg = r["result"]
        view = find_view(cfg)

        # Karten: Zoom-Karten ersetzen/ergaenzen
        charts = {
            "zoom-temp": zoom_card("Temperaturen im Verlauf", TEMPS, "24h", 260),
            "zoom-energy": zoom_card("Energie im Verlauf", ENERGY, "30T", 260),
            "zoom-leistung": zoom_card("Leistung & COP im Verlauf", LEISTUNG, "24h", 240),
        }

        # Sektionen: aktuelle Werte, heute, zaehlerstaende, dann Charts
        sections = view.get("sections", [])
        # Chart-Sektion am Ende sicherstellen
        chart_sec = None
        for s in sections:
            for c in s.get("cards", []):
                if c.get("type") == "custom:wp-zoom-chart":
                    chart_sec = s
        if chart_sec is None:
            chart_sec = {"type": "grid", "column_span": 3, "cards": [
                {"type": "heading", "heading": "Verlauf",
                 "heading_style": "title", "icon": "mdi:chart-timeline-variant"},
            ]}
            sections.append(chart_sec)
        have = {c.get("title") for c in chart_sec["cards"]
                if c.get("type") == "custom:wp-zoom-chart"}
        for k, card in charts.items():
            if card["title"] not in have:
                chart_sec["cards"].append(card)

        view["sections"] = sections
        r = await call(4, {"type": "lovelace/config/save",
                           "url_path": "waermepumpe-dashboard", "config": cfg})
        print("Dashboard gespeichert:", r.get("success"), r.get("error") or "")

        # 4) Kontrolle
        r = await call(5, {"type": "lovelace/config",
                           "url_path": "waermepumpe-dashboard"})
        v = r["result"]["views"][0]
        for i, s in enumerate(v.get("sections", [])):
            types = [c.get("type") for c in s.get("cards", [])]
            print(f"  section {i}: {types}")
        r = await call(6, {"type": "lovelace/resources"})
        print("Ressourcen:", [x.get("url") for x in r.get("result", [])])


asyncio.run(main())
