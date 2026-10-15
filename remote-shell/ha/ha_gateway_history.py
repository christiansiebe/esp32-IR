#!/usr/bin/env python3
"""Backfill a heat-pump gateway's own recordings into Home Assistant.

A Bosch/Buderus heat-pump gateway (K40RF local API, ``/recordings/<path>``) keeps
its own long-term history: daily/monthly buckets going back far beyond what Home
Assistant's recorder holds. HA only starts recording when the integration is
configured, so everything before that date is missing from the HA database even
though the gateway still has it.

This tool pulls the gateway recordings and imports the buckets that precede HA's
own first sample into HA long-term statistics, via the WebSocket command
``recorder/import_statistics`` (there is no REST endpoint for it).

Design notes
------------
* Only buckets **older** than the first HA-owned statistic point are imported, so
  the import never overlaps or overwrites live data.
* Monthly buckets (``P1M``) fill the months before the daily range; daily buckets
  (``P1D``) fill everything up to HA's first sample.
* The command is idempotent: re-running it skips days that already exist.

Usage (secrets stay in the environment / HA .storage, never on the CLI):

  docker exec homeassistant python3 /config/ha_gateway_history.py

Requires ``pyk40rf`` (the same client the ``bosch_k40rf`` integration uses) and
runs **inside** the Home Assistant container so it can read
``/config/.storage/core.config_entries`` (gateway host/token) and
``/config/.storage/auth`` (a long-lived token to mint an admin JWT).
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import sqlite3
import time

DB = "/config/home-assistant_v2.db"
CE = "/config/.storage/core.config_entries"
AUTH = "/config/.storage/auth"
WS_URL = "ws://127.0.0.1:8123/api/websocket"

# Gateway recording path -> HA statistic_id (a sensor created by the integration).
DEFAULT_MAP = {
    "/dhwCircuits/dhw1/actualTemp": "sensor.warmwasser_actual_temperature",
    "/heatSources/actualSupplyTemperature": "sensor.mx_400_supply_temperature",
    "/system/sensors/temperatures/outdoor_t1": "sensor.mx_400_outdoor_temperature",
}


def gateway_credentials():
    entries = json.load(open(CE))["data"]["entries"]
    e = next(x for x in entries if x["domain"] == "bosch_k40rf")
    return e["data"]["host"], e["data"]["port"], e["data"]["token"]


def admin_token() -> str:
    """Mint a short-lived admin JWT from HA's stored long-lived token."""
    import jwt
    data = json.load(open(AUTH))
    rt = [t for t in data["data"]["refresh_tokens"]
          if t.get("token_type") == "long_lived_access_token"][0]
    now = int(time.time())
    tok = jwt.encode({"iss": rt["id"], "iat": now, "exp": now + 1800},
                     rt["jwt_key"], algorithm="HS256")
    return tok.decode() if isinstance(tok, bytes) else tok


def first_ha_sample(statistic_id: str):
    """Epoch seconds of HA's earliest stored point for this statistic, or None."""
    con = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    try:
        cur = con.cursor()
        cur.execute(
            """SELECT MIN(s.start_ts) FROM statistics s
               JOIN statistics_meta m ON m.id = s.metadata_id
               WHERE m.statistic_id = ?""", (statistic_id,))
        row = cur.fetchone()
        return row[0] if row else None
    finally:
        con.close()


async def fetch_gateway(host, port, token, path):
    import aiohttp
    from pyk40rf import K40Client
    async with aiohttp.ClientSession() as sess:
        cli = K40Client(host, sess, data_port=port, token=token, verify_ssl=False)
        rec = await cli.async_get_recording(f"/recordings{path}", sample_rate="all")
        daily, monthly = {}, {}
        for series in rec.series:
            for bucket in series.buckets:
                if bucket.average is None or bucket.start is None:
                    continue
                key = bucket.start.astimezone(dt.timezone.utc)
                if series.sample_rate == "P1D":
                    daily[key] = round(bucket.average, 3)
                elif series.sample_rate == "P1M":
                    monthly[key] = round(bucket.average, 3)
        return daily, monthly


def build_points(daily, monthly, cutoff):
    """Daily buckets before HA's first sample, plus months before the daily range.

    Monthly buckets only fill the span *before* the first daily bucket's month, so
    a month is never imported alongside the days it already summarises. If there
    is no daily bucket at all, the cutoff alone bounds the months.
    """
    stats = []
    for start, val in sorted(daily.items()):
        if cutoff is None or start.timestamp() < cutoff - 1:
            stats.append({"start": start.isoformat(), "mean": val})
    if daily:
        first = min(daily)
        month_anchor = dt.datetime(first.year, first.month, 1, tzinfo=first.tzinfo)
    elif cutoff is not None:
        month_anchor = dt.datetime.fromtimestamp(cutoff, dt.timezone.utc)
    else:
        month_anchor = None
    for start, val in sorted(monthly.items()):
        if month_anchor is None or start < month_anchor:
            stats.append({"start": start.isoformat(), "mean": val})
    return stats


async def import_stats(ws, mid, statistic_id, stats, unit, unit_class):
    meta = {"statistic_id": statistic_id, "source": "recorder", "name": None,
            "unit_of_measurement": unit, "has_mean": True, "has_sum": False,
            "mean_type": 1, "unit_class": unit_class}
    await ws.send(json.dumps({"id": mid, "type": "recorder/import_statistics",
                              "metadata": meta, "stats": stats}))
    while True:
        msg = json.loads(await ws.recv())
        if msg.get("id") == mid and msg.get("type") == "result":
            return msg


async def run(mapping, dry_run):
    import websockets
    host, port, token = gateway_credentials()
    gw = {}
    for path, sid in mapping.items():
        gw[sid] = await fetch_gateway(host, port, token, path)
        daily, monthly = gw[sid]
        print(f"{sid}: {len(daily)} daily, {len(monthly)} monthly buckets")

    async with websockets.connect(WS_URL, max_size=64 * 1024 * 1024) as ws:
        await ws.recv()
        await ws.send(json.dumps({"type": "auth", "access_token": admin_token()}))
        hello = json.loads(await ws.recv())
        if hello.get("type") != "auth_ok":
            raise SystemExit(f"auth failed: {hello}")

        mid = 100
        for sid, (daily, monthly) in gw.items():
            cutoff = first_ha_sample(sid)
            stats = build_points(daily, monthly, cutoff)
            span = (f"{stats[0]['start'][:10]}..{stats[-1]['start'][:10]}"
                    if stats else "-")
            print(f"\n{sid}: {len(stats)} points to import ({span})")
            if not stats:
                continue
            if dry_run:
                print("  dry-run: not sending")
                continue
            mid += 1
            res = await import_stats(ws, mid, sid, stats, "°C", "temperature")
            print("  import:", res.get("success"), res.get("error") or "")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true",
                    help="show what would be imported without sending")
    ap.add_argument("--map", help="JSON object of {recording_path: statistic_id}")
    args = ap.parse_args()
    mapping = json.loads(args.map) if args.map else DEFAULT_MAP
    asyncio.run(run(mapping, args.dry_run))


if __name__ == "__main__":
    main()
