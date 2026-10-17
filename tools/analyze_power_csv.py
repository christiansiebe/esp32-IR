#!/usr/bin/env python3
"""Analyse a receiver power-history CSV export for missing minutes.

Input: the file downloaded from the receiver at ``/api/power-history.csv``.
Columns: ``sequence,interval_start_ms,average_w,sample_count`` (CRLF, UTF-8).
``interval_start_ms`` is an ISO-8601 UTC timestamp even though the column name
still says ``_ms`` (the handler emits ``%Y-%m-%dT%H:%M:%SZ``).

The goal is the project requirement "no data loss": every gap in the minute
sequence means minutes the receiver never stored. This script distinguishes

  * time gaps   - consecutive intervals more than one minute apart (real loss),
  * sequence gaps - the record sequence jumped (transport loss vs. reorder),
  * thin minutes - a stored minute with an unusually low sample_count,
  * duplicates  - the same interval stored twice.

Usage:  python3 tools/analyze_power_csv.py power-history.csv [--interval 60]
"""

from __future__ import annotations

import argparse
import csv
import sys
from datetime import datetime, timezone


def parse_timestamp(value: str) -> datetime | None:
    value = value.strip()
    for fmt in ("%Y-%m-%dT%H:%M:%SZ", "%Y-%m-%dT%H:%M:%S"):
        try:
            return datetime.strptime(value, fmt).replace(tzinfo=timezone.utc)
        except ValueError:
            continue
    return None


def load(path: str) -> list[dict]:
    with open(path, newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        required = {"sequence", "interval_start_ms", "average_w", "sample_count"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            raise SystemExit(
                f"unexpected header {reader.fieldnames!r}; missing {sorted(missing)}"
            )
        rows = []
        for line_no, row in enumerate(reader, start=2):
            ts = parse_timestamp(row["interval_start_ms"])
            if ts is None:
                print(f"warning: line {line_no}: unparsable timestamp "
                      f"{row['interval_start_ms']!r}, skipped", file=sys.stderr)
                continue
            rows.append(
                {
                    "sequence": int(row["sequence"]),
                    "start": ts,
                    "watts": float(row["average_w"]),
                    "samples": int(row["sample_count"]),
                    "line": line_no,
                }
            )
    return rows


def analyse(rows: list[dict], interval: int) -> int:
    if not rows:
        print("no usable rows in the file")
        return 1

    rows.sort(key=lambda r: r["start"])
    span = rows[-1]["start"] - rows[0]["start"]
    expected = int(span.total_seconds() // interval) + 1

    print(f"records stored : {len(rows)}")
    print(f"first minute   : {rows[0]['start'].isoformat()}")
    print(f"last minute    : {rows[-1]['start'].isoformat()}")
    print(f"time span      : {span} ({expected} minutes at {interval}s)")
    print(f"missing minutes: {expected - len(rows)}")

    time_gaps = []
    for prev, cur in zip(rows, rows[1:]):
        delta = int((cur["start"] - prev["start"]).total_seconds())
        if delta > interval:
            time_gaps.append((prev["start"], cur["start"], delta // interval - 1))

    print(f"\ntime gaps (> {interval}s between stored minutes): {len(time_gaps)}")
    for start, end, count in time_gaps:
        print(f"  {start.isoformat()} -> {end.isoformat()}  "
              f"{count} minute(s) lost")

    seq_gaps = []
    for prev, cur in zip(rows, rows[1:]):
        step = cur["sequence"] - prev["sequence"]
        if step != 1:
            kind = "backward/reorder" if step < 1 else "forward loss"
            seq_gaps.append((prev["sequence"], cur["sequence"], step, kind))
    print(f"\nsequence gaps   : {len(seq_gaps)}")
    for a, b, step, kind in seq_gaps:
        print(f"  seq {a} -> {b} (step {step}, {kind})")

    seen: dict[datetime, int] = {}
    duplicates = 0
    for row in rows:
        seen[row["start"]] = seen.get(row["start"], 0) + 1
    duplicates = sum(1 for count in seen.values() if count > 1)
    print(f"\nduplicate minutes: {duplicates}")

    samples = [r["samples"] for r in rows]
    median = sorted(samples)[len(samples) // 2]
    thin = [r for r in rows if r["samples"] < max(1, median // 2)]
    print(f"\nsample_count     : min {min(samples)}  median {median}  "
          f"max {max(samples)}")
    print(f"thin minutes (< half median): {len(thin)}")
    for row in thin[:20]:
        print(f"  {row['start'].isoformat()}  samples={row['samples']}  "
              f"{row['watts']:.3f} W")
    if len(thin) > 20:
        print(f"  ... and {len(thin) - 20} more")

    total_lost = sum(count for _, _, count in time_gaps)
    print("\nsummary:")
    print(f"  real lost minutes (time gaps): {total_lost}")
    print(f"  sequence-only anomalies      : {len(seq_gaps)}")
    print(f"  duplicate minutes            : {duplicates}")
    print(f"  thin minutes                 : {len(thin)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", help="path to power-history.csv")
    parser.add_argument("--interval", type=int, default=60,
                        help="bucket size in seconds (default 60)")
    args = parser.parse_args()
    return analyse(load(args.csv), args.interval)


if __name__ == "__main__":
    raise SystemExit(main())
