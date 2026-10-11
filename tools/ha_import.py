#!/usr/bin/env python3
"""Import esp32-IR power history into Home Assistant long-term statistics.

Home Assistant does not pull history over MQTT, and ``recorder/import_statistics``
is only available over the WebSocket API, so this bridge:

  1. pulls the raw minute records from the device (``/api/power-history?after=``),
  2. aggregates them into hourly buckets (tools/ha_stats.py),
  3. pushes them into HA with the ``recorder/import_statistics`` WebSocket command.

Usage (environment variables keep secrets out of the command line):

  ESP32IR_URL=http://192.168.4.1 \
  HA_URL=http://homeassistant.local:8123 \
  HA_TOKEN=<long-lived-access-token> \
  python3 tools/ha_import.py --statistic-id esp32ir:meter_energy --unit kWh

Options:
  --power-too        also import an hourly mean power statistic (W)
  --dry-run          print the payload instead of sending it
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ha_stats  # noqa: E402
import ws_min  # noqa: E402


def fetch_json(url: str, timeout: float = 30.0) -> dict:
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def fetch_all_records(base_url: str) -> list[dict]:
    """Page through /api/power-history until every stored record is collected."""
    records: list[dict] = []
    cursor = 0
    while True:
        url = "%s/api/power-history?after=%d" % (base_url.rstrip("/"), cursor)
        payload = fetch_json(url)
        page = ha_stats.parse_records(payload)
        if not page:
            break
        records.extend(page)
        newest = max(record["sequence"] for record in page)
        if newest <= cursor:
            break
        cursor = newest
    return records


def ha_version(base_url: str, token: str) -> str:
    request = urllib.request.Request(
        "%s/api/config" % base_url.rstrip("/"),
        headers={"Authorization": "Bearer %s" % token},
    )
    try:
        with urllib.request.urlopen(request, timeout=15.0) as response:
            return json.loads(response.read().decode("utf-8")).get("version", "0.0")
    except urllib.error.HTTPError as error:
        raise SystemExit("Home Assistant rejected the token (HTTP %s)" % error.code)


def send_statistic(ws_host, ws_port, token, message, dry_run):
    if dry_run:
        print(json.dumps(message, indent=2))
        return
    client = ws_min.Client(ws_host, ws_port).connect()
    try:
        auth_required = client.recv_json()
        if auth_required.get("type") != "auth_required":
            raise SystemExit("unexpected greeting: %s" % auth_required)
        client.send_json({"type": "auth", "access_token": token})
        auth_result = client.recv_json()
        if auth_result.get("type") != "auth_ok":
            raise SystemExit("Home Assistant authentication failed")
        client.send_json(message)
        reply = client.recv_json()
        if not reply.get("success"):
            raise SystemExit("import rejected: %s" % reply.get("error"))
    finally:
        client.close()


def parse_host_port(base_url: str) -> tuple[str, int]:
    without_scheme = base_url.split("://", 1)[-1]
    host_port = without_scheme.split("/", 1)[0]
    if ":" in host_port:
        host, port = host_port.split(":", 1)
        return host, int(port)
    return host_port, 8123


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--esp32ir-url",
                        default=os.environ.get("ESP32IR_URL", "http://192.168.4.1"))
    parser.add_argument("--ha-url", default=os.environ.get("HA_URL", ""))
    parser.add_argument("--ha-token", default=os.environ.get("HA_TOKEN", ""))
    parser.add_argument("--statistic-id", default="esp32ir:meter_energy")
    parser.add_argument("--power-statistic-id", default="esp32ir:meter_power")
    parser.add_argument("--unit", default="kWh")
    parser.add_argument("--power-too", action="store_true",
                        help="also import an hourly mean power statistic")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not args.ha_url and not args.dry_run:
        raise SystemExit("set HA_URL or use --dry-run")

    print("Fetching records from %s ..." % args.esp32ir_url)
    records = fetch_all_records(args.esp32ir_url)
    if not records:
        print("No stored records on the device; nothing to import.")
        return 0
    print("Fetched %d minute records." % len(records))

    hourly = ha_stats.with_cumulative_energy(ha_stats.aggregate_hourly(records))
    print("Aggregated into %d hourly buckets." % len(hourly))

    version = "0.0" if args.dry_run else ha_version(args.ha_url, args.ha_token)
    host, port = parse_host_port(args.ha_url or "http://localhost:8123")

    energy = ha_stats.build_import_message(
        1, version, args.statistic_id, args.unit, hourly,
        name="esp32-IR meter energy", has_sum=True, has_mean=False,
    )
    send_statistic(host, port, args.ha_token, energy, args.dry_run)
    print("Imported energy statistic %s." % args.statistic_id)

    if args.power_too:
        power = ha_stats.build_import_message(
            2, version, args.power_statistic_id, "W", hourly,
            name="esp32-IR meter power", has_sum=False, has_mean=True,
        )
        send_statistic(host, port, args.ha_token, power, args.dry_run)
        print("Imported power statistic %s." % args.power_statistic_id)

    return 0


if __name__ == "__main__":
    sys.exit(main())
