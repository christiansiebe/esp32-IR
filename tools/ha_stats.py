"""Pure helpers to turn esp32-IR power-history records into Home Assistant
long-term statistics import payloads.

No I/O here on purpose: everything in this module is deterministic and covered
by test/test_ha_stats.py. The network/WebSocket plumbing lives in ha_import.py.

Background: Home Assistant fills its recorder from live state changes. It does
NOT pull history over MQTT, so the records already stored on the device have to
be pushed in with the ``recorder/import_statistics`` WebSocket command (there is
no REST endpoint for this). That command wants hourly buckets with a real UTC
timestamp, which is exactly what we build here.
"""

from __future__ import annotations

import datetime as _dt
from typing import Iterable, Sequence


def parse_records(payload: dict) -> list[dict]:
    """Extract the raw minute records from a /api/power-history JSON response."""
    records = payload.get("records") or []
    parsed = []
    for item in records:
        if not isinstance(item, dict):
            continue
        timestamp_ms = item.get("timestamp")
        watts = item.get("watts")
        if timestamp_ms is None or watts is None:
            continue
        parsed.append(
            {
                "timestamp_ms": int(timestamp_ms),
                "watts": float(watts),
                "sample_count": int(item.get("sample_count") or 0),
                "sequence": int(item.get("sequence") or 0),
            }
        )
    return parsed


def _hour_start(epoch_seconds: int) -> int:
    return epoch_seconds - (epoch_seconds % 3600)


def _iso_utc(epoch_seconds: int) -> str:
    moment = _dt.datetime.fromtimestamp(epoch_seconds, tz=_dt.timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%S+00:00")


def aggregate_hourly(records: Iterable[dict]) -> list[dict]:
    """Group minute records into hourly buckets.

    The mean power of a bucket is weighted by each record's sample count, so a
    minute that carried fewer samples does not weigh the same as a full one.
    Energy is derived from the mean power over the hour (mean W * 1 h = Wh).
    """
    buckets: dict[int, dict] = {}
    for record in records:
        epoch = record["timestamp_ms"] // 1000
        start = _hour_start(epoch)
        bucket = buckets.setdefault(start, {"weight": 0, "watt_seconds": 0.0})
        weight = record["sample_count"] or 1
        bucket["weight"] += weight
        bucket["watt_seconds"] += record["watts"] * weight

    result = []
    for start in sorted(buckets):
        bucket = buckets[start]
        mean_w = bucket["watt_seconds"] / bucket["weight"]
        result.append(
            {
                "start": _iso_utc(start),
                "start_epoch": start,
                "mean_w": mean_w,
                "energy_kwh": mean_w / 1000.0,
                "weight": bucket["weight"],
            }
        )
    return result


def with_cumulative_energy(hourly: Sequence[dict]) -> list[dict]:
    """Add a monotonically increasing ``sum_kwh`` (energy dashboard needs it)."""
    running = 0.0
    enriched = []
    for bucket in hourly:
        running += bucket["energy_kwh"]
        item = dict(bucket)
        item["sum_kwh"] = running
        enriched.append(item)
    return enriched


def parse_version(version: str) -> tuple[int, int]:
    """Parse a Home Assistant version string into (major, minor)."""
    parts = str(version).split(".")
    try:
        major = int(parts[0])
        minor = int(parts[1]) if len(parts) > 1 else 0
    except (ValueError, IndexError):
        return (0, 0)
    return (major, minor)


def build_metadata(
    ha_version: str,
    statistic_id: str,
    unit_of_measurement: str,
    *,
    name: str,
    has_sum: bool,
    has_mean: bool,
) -> dict:
    """Build the metadata block, adapting to the HA statistics API version.

    HA 2025.10 replaced ``has_mean`` with ``mean_type`` and added ``unit_class``.
    Sending the old field to a new server (or vice versa) is rejected, so pick
    the shape that matches the target version.
    """
    metadata = {
        "statistic_id": statistic_id,
        "name": name,
        "source": statistic_id.split(":", 1)[0],
        "unit_of_measurement": unit_of_measurement,
        "has_sum": has_sum,
    }
    if parse_version(ha_version) >= (2025, 10):
        # mean_type: 0 = none, 1 = arithmetic. unit_class is required now.
        metadata["mean_type"] = 1 if has_mean else 0
        metadata["unit_class"] = "power" if has_mean else "energy"
    else:
        metadata["has_mean"] = has_mean
    return metadata


def build_stats(hourly: Sequence[dict], *, has_sum: bool, has_mean: bool) -> list[dict]:
    """Build the per-hour ``stats`` list for recorder/import_statistics."""
    stats = []
    for bucket in hourly:
        item = {"start": bucket["start"]}
        if has_mean:
            item["mean"] = round(bucket["mean_w"], 3)
            item["min"] = round(bucket["mean_w"], 3)
            item["max"] = round(bucket["mean_w"], 3)
        if has_sum:
            # Energy dashboard treats this as total_increasing; state mirrors sum.
            item["state"] = round(bucket["sum_kwh"], 6)
            item["sum"] = round(bucket["sum_kwh"], 6)
        stats.append(item)
    return stats


def build_import_message(
    message_id: int,
    ha_version: str,
    statistic_id: str,
    unit_of_measurement: str,
    hourly: Sequence[dict],
    *,
    name: str,
    has_sum: bool,
    has_mean: bool,
) -> dict:
    """Assemble the full recorder/import_statistics WebSocket message."""
    return {
        "id": message_id,
        "type": "recorder/import_statistics",
        "metadata": build_metadata(
            ha_version,
            statistic_id,
            unit_of_measurement,
            name=name,
            has_sum=has_sum,
            has_mean=has_mean,
        ),
        "stats": build_stats(hourly, has_sum=has_sum, has_mean=has_mean),
    }
