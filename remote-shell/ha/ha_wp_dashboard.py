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
import hashlib
import json
import os
import shutil
import time

import jwt
import websockets


def resource_url(path):
    """Append a content hash so the browser cannot serve a stale copy.

    HA serves ``/local/*`` with ``Cache-Control: max-age=2678400`` (31 days),
    and a resource URL without a query string is cached forever. Without this
    the deployed card changes never reach the browser.
    """
    with open(path, "rb") as fh:
        ver = hashlib.sha256(fh.read()).hexdigest()[:10]
    return "/local/wp-zoom-chart.js?v=" + ver

AUTH = "/config/.storage/auth"
DASH = "/config/.storage/lovelace.waermepumpe_dashboard"
TEMPLATES = "/config/templates.yaml"
RES_URL = "/local/wp-zoom-chart.js"
URL = "ws://127.0.0.1:8123/api/websocket"

# The heat source reports actualModulation as a constant 0, so derive the
# modulation from the compressor's electrical power against the 3 kW rating of
# the connection. See the module docstring / SHARED.md for the evidence.
MOD_START = "# >>> WP-MODULATION (auto) <<<"
MOD_END = "# <<< WP-MODULATION (auto) <<<"
MOD_ENTITY = "sensor.wp_modulation"
MOD_RATING_W = 3000

MODULATION_BLOCK = MOD_START + """
- sensor:
    - name: "WP Modulation"
      unique_id: wp_modulation
      unit_of_measurement: "%"
      state_class: measurement
      icon: mdi:percent
      state: >
        {% set p = states('sensor.mx_400_compressor_electrical_power') | float(0) %}
        {% set m = p * 100 / """ + str(MOD_RATING_W) + """ %}
        {{ (m if m < 100 else 100) | round(0) }}
""" + MOD_END + "\n"


def upsert_marker_block(text, start, end, block):
    """Replace the block between ``start`` and ``end`` markers, else append it.

    Idempotent: running twice yields the same text. Used for the generated
    template-sensor sections so a redeploy never duplicates or drops them.
    """
    if start in text and end in text:
        pre = text.split(start)[0].rstrip()
        post = text.split(end)[-1].lstrip("\n")
        return pre + "\n" + block + post
    return text.rstrip() + "\n" + block



def admin_token():
    """Mint a short-lived admin JWT from HA's stored refresh token."""
    d = json.load(open(AUTH))
    rt = [t for t in d["data"]["refresh_tokens"]
          if t.get("token_type") == "long_lived_access_token"][0]
    now = int(time.time())
    access = jwt.encode({"iss": rt["id"], "iat": now, "exp": now + 900},
                        rt["jwt_key"], algorithm="HS256")
    return access.decode() if isinstance(access, bytes) else access

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


def normalize_apex_card(card):
    """Make a HACS ``apexcharts-card`` resizable in HA's sections view.

    The card only returns grid options (``getGridOptions`` -> rows/columns) when
    ``section_mode`` is set, and it forces ``chart.height = "100%"`` in that
    mode. A fixed ``apex_config.chart.height`` is merged *after* and would pin
    the height again, so it is removed. Returns True when something changed.
    """
    if card.get("type") != "custom:apexcharts-card":
        return False
    changed = False
    if card.get("section_mode") is not True:
        card["section_mode"] = True
        changed = True
    apex = card.get("apex_config")
    chart = apex.get("chart") if isinstance(apex, dict) else None
    if isinstance(chart, dict) and "height" in chart:
        del chart["height"]
        changed = True
    return changed


def normalize_cards(node):
    """Apply normalize_apex_card to every card, recursing through the config."""
    changed = False
    if isinstance(node, dict):
        if node.get("type") == "custom:apexcharts-card":
            changed = normalize_apex_card(node) or changed
        for value in node.values():
            if isinstance(value, (dict, list)):
                changed = normalize_cards(value) or changed
    elif isinstance(node, list):
        for child in node:
            changed = normalize_cards(child) or changed
    return changed


def wrap_expand_card(card, title=""):
    """Wrap a card in ``custom:wp-expand-card`` so it gets the expand button.

    The HACS ``apexcharts-card`` cannot be enlarged itself and brings no such
    button. Wrapping it in our own card renders it unchanged but adds the same
    expand control the zoom charts have (click -> large, resizable overlay).
    Idempotent: an already wrapped card is returned as-is.
    """
    if card.get("type") == "custom:wp-expand-card":
        return card
    inner = dict(card)
    return {"type": "custom:wp-expand-card", "title": title, "card": inner}


def wrap_all_apex(node, title=""):
    """Wrap every custom:apexcharts-card found in the config. Returns count."""
    count = 0
    if isinstance(node, dict):
        for key, value in list(node.items()):
            if isinstance(value, list):
                for i, child in enumerate(value):
                    if isinstance(child, dict) and child.get("type") == "custom:apexcharts-card":
                        value[i] = wrap_expand_card(child, title)
                        count += 1
                    else:
                        count += wrap_all_apex(child, title)
            elif isinstance(value, dict):
                count += wrap_all_apex(value, title)
    elif isinstance(node, list):
        for child in node:
            count += wrap_all_apex(child, title)
    return count


def write_modulation_template():
    """Add the derived modulation sensor to templates.yaml (idempotent)."""
    if not os.path.exists(TEMPLATES):
        print("templates.yaml fehlt – Modulation uebersprungen")
        return
    text = open(TEMPLATES).read()
    new = upsert_marker_block(text, MOD_START, MOD_END, MODULATION_BLOCK)
    if new != text:
        open(TEMPLATES, "w").write(new)
        print("templates.yaml: WP-Modulation Block aktualisiert")
    else:
        print("templates.yaml: WP-Modulation Block aktuell")


def ensure_reference_gauges(view):
    """Match the Bosch reference layout: electrical / thermal / efficiency.

    The referenced ``simple-dashboard.yaml`` shows three gauges for electrical
    intake, thermal output and efficiency. Our dashboard already carries the
    first and last; add the thermal-output gauge next to the compressor gauge.
    Idempotent.
    """
    thermal = "sensor.buderus_thermische_leistung_heizung"
    changed = False
    for s in view.get("sections", []):
        for i, c in enumerate(s.get("cards", [])):
            if c.get("entity") == "sensor.mx_400_compressor_electrical_power":
                have = [x.get("entity") for x in s["cards"]]
                if thermal not in have:
                    s["cards"].insert(i + 1, {
                        "type": "gauge", "entity": thermal,
                        "name": "Thermische Leistung", "min": 0, "max": 15,
                        "unit": "kW", "needle": True, "max_decimals": 1})
                    changed = True
                if MOD_ENTITY not in have and not any(
                        c2.get("entity") == MOD_ENTITY for c2 in s["cards"]):
                    s["cards"].insert(i, {
                        "type": "gauge", "entity": MOD_ENTITY,
                        "name": "Modulation (berechnet)", "min": 0, "max": 100,
                        "needle": True, "unit": "%"})
                    changed = True
                return changed
    return changed


def replace_raw_modulation_tile(view):
    """Point the always-zero tile at the derived sensor instead.

    ``sensor.mx_400_actual_modulation`` is a constant 0 from the gateway, so the
    tile is misleading. Swap the entity but keep the user's card/name styling.
    Returns True when the tile was changed.
    """
    changed = False
    for s in view.get("sections", []):
        for c in s.get("cards", []):
            if c.get("entity") == "sensor.mx_400_actual_modulation":
                c["entity"] = MOD_ENTITY
                c.setdefault("name", "Modulation (berechnet)")
                changed = True
    return changed


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
        await ws.send(json.dumps({"type": "auth", "access_token": admin_token()}))
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

        # 2) Ressource registrieren (mit Inhalts-Hash gegen den 31-Tage-Cache)
        want = resource_url(dst) if os.path.exists(dst) else RES_URL
        r = await call(1, {"type": "lovelace/resources"})
        items = r.get("result") or []
        cur = [x for x in items if x.get("url", "").split("?")[0] == RES_URL]
        if not cur:
            r = await call(2, {"type": "lovelace/resources/create",
                               "res_type": "module", "url": want})
            print("Ressource erstellt:", r.get("success"), r.get("error") or "", want)
        elif cur[0].get("url") != want:
            r = await call(2, {"type": "lovelace/resources/update",
                               "resource_id": cur[0]["id"],
                               "res_type": "module", "url": want})
            print("Ressource aktualisiert:", r.get("success"), r.get("error") or "", want)
        else:
            print("Ressource aktuell:", want)

        # 3) Dashboard laden
        r = await call(3, {"type": "lovelace/config",
                           "url_path": "waermepumpe-dashboard"})
        cfg = r["result"]
        view = find_view(cfg)

        # HACS apexcharts-card resizable machen (section_mode + feste Hoehe weg)
        if normalize_cards(cfg):
            print("apexcharts-card fuer Grid-Resize normalisiert")

        # Jede apexcharts-card in unser Hüllen-Card packen -> ⛶-Grossansicht
        n = wrap_all_apex(cfg)
        if n:
            print(f"{n} apexcharts-card(s) in wp-expand-card verpackt")

        # Abgeleitete Modulation (Quelle meldet konstant 0) + Anzeige
        write_modulation_template()
        if replace_raw_modulation_tile(view):
            print("Immer-0-Kachel auf berechnete Modulation umgestellt")
        if ensure_reference_gauges(view):
            print("Referenz-Gauges (Modulation/thermisch) ergaenzt")

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
        v = (r.get("result") or {}).get("views", [{}])[0]
        for i, s in enumerate(v.get("sections", [])):
            types = [c.get("type") for c in s.get("cards", [])]
            print(f"  section {i}: {types}")
        r = await call(6, {"type": "lovelace/resources"})
        print("Ressourcen:", [x.get("url") for x in r.get("result", [])])

        # 5) Templates neu laden, damit sensor.wp_modulation ohne HA-Neustart lebt
        r = await call(7, {"type": "call_service", "domain": "template",
                           "service": "reload"})
        print("Template-Reload:", r.get("success"), r.get("error") or "")


if __name__ == "__main__":
    asyncio.run(main())
