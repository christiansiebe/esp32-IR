"""Host tests for tools/ha_stats.py (pure, no network)."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import ha_stats  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def test_parse_records_skips_bad_entries():
    print("test_parse_records_skips_bad_entries")
    payload = {
        "records": [
            {"sequence": 1, "timestamp": 1_700_000_000_000, "watts": 100.0, "sample_count": 60},
            {"sequence": 2, "watts": 50.0},          # missing timestamp
            {"sequence": 3, "timestamp": 1_700_000_060_000},  # missing watts
            "not-a-dict",
        ]
    }
    parsed = ha_stats.parse_records(payload)
    check(len(parsed) == 1, "only complete records survive")
    check(parsed[0]["timestamp_ms"] == 1_700_000_000_000, "timestamp kept")
    check(parsed[0]["watts"] == 100.0, "watts kept")


def test_aggregate_hourly_weighted_mean():
    print("test_aggregate_hourly_weighted_mean")
    # Two minutes in the same hour, different sample counts.
    base = 1_700_000_000 - (1_700_000_000 % 3600)
    records = [
        {"timestamp_ms": base * 1000, "watts": 1000.0, "sample_count": 60},
        {"timestamp_ms": (base + 60) * 1000, "watts": 0.0, "sample_count": 20},
    ]
    hourly = ha_stats.aggregate_hourly(records)
    check(len(hourly) == 1, "both minutes collapse into one hour")
    # weighted: (1000*60 + 0*20) / 80 = 750
    check(abs(hourly[0]["mean_w"] - 750.0) < 1e-9, "sample-count weighted mean")
    check(abs(hourly[0]["energy_kwh"] - 0.75) < 1e-9, "energy = mean W / 1000")
    check(hourly[0]["start"].endswith("+00:00"), "UTC offset present")


def test_aggregate_splits_hours_and_sorts():
    print("test_aggregate_splits_hours_and_sorts")
    base = 1_700_000_000 - (1_700_000_000 % 3600)
    records = [
        {"timestamp_ms": (base + 3660) * 1000, "watts": 200.0, "sample_count": 60},
        {"timestamp_ms": base * 1000, "watts": 100.0, "sample_count": 60},
    ]
    hourly = ha_stats.aggregate_hourly(records)
    check(len(hourly) == 2, "two different hours")
    check(hourly[0]["start_epoch"] < hourly[1]["start_epoch"], "sorted ascending")


def test_cumulative_energy_monotonic():
    print("test_cumulative_energy_monotonic")
    hourly = [
        {"start": "2024-01-01T00:00:00+00:00", "start_epoch": 1, "mean_w": 1000.0, "energy_kwh": 1.0, "weight": 60},
        {"start": "2024-01-01T01:00:00+00:00", "start_epoch": 2, "mean_w": 2000.0, "energy_kwh": 2.0, "weight": 60},
    ]
    enriched = ha_stats.with_cumulative_energy(hourly)
    check(abs(enriched[0]["sum_kwh"] - 1.0) < 1e-9, "first sum")
    check(abs(enriched[1]["sum_kwh"] - 3.0) < 1e-9, "second sum accumulates")


def test_metadata_old_api():
    print("test_metadata_old_api")
    meta = ha_stats.build_metadata(
        "2025.9.3", "esp32ir:meter_energy", "kWh",
        name="Meter energy", has_sum=True, has_mean=False,
    )
    check(meta["has_mean"] is False, "old API uses has_mean")
    check("mean_type" not in meta, "old API has no mean_type")
    check(meta["source"] == "esp32ir", "source from statistic_id domain")


def test_metadata_new_api():
    print("test_metadata_new_api")
    meta = ha_stats.build_metadata(
        "2025.10.0", "esp32ir:meter_energy", "kWh",
        name="Meter energy", has_sum=True, has_mean=False,
    )
    check("has_mean" not in meta, "new API drops has_mean")
    check(meta["mean_type"] == 0, "mean_type none for energy")
    check(meta["unit_class"] == "energy", "unit_class energy")

    power = ha_stats.build_metadata(
        "2026.1.0", "esp32ir:meter_power", "W",
        name="Meter power", has_sum=False, has_mean=True,
    )
    check(power["mean_type"] == 1, "mean_type arithmetic for power")
    check(power["unit_class"] == "power", "unit_class power")


def test_build_stats_shapes():
    print("test_build_stats_shapes")
    hourly = ha_stats.with_cumulative_energy([
        {"start": "2024-01-01T00:00:00+00:00", "start_epoch": 1, "mean_w": 1234.5, "energy_kwh": 1.2345, "weight": 60},
    ])
    stats = ha_stats.build_stats(hourly, has_sum=True, has_mean=True)
    check(stats[0]["mean"] == 1234.5, "mean rounded to 3 dp")
    check(stats[0]["min"] == stats[0]["max"] == stats[0]["mean"], "min/max present")
    check(abs(stats[0]["sum"] - 1.2345) < 1e-6, "sum present")

    energy_only = ha_stats.build_stats(hourly, has_sum=True, has_mean=False)
    check("mean" not in energy_only[0], "no mean when has_mean is false")
    check("sum" in energy_only[0], "sum present for energy")


def test_full_message_round_trip():
    print("test_full_message_round_trip")
    base = 1_700_000_000 - (1_700_000_000 % 3600)
    records = [{"timestamp_ms": base * 1000, "watts": 500.0, "sample_count": 60}]
    hourly = ha_stats.with_cumulative_energy(ha_stats.aggregate_hourly(records))
    message = ha_stats.build_import_message(
        7, "2026.1.0", "esp32ir:meter_energy", "kWh", hourly,
        name="Meter energy", has_sum=True, has_mean=False,
    )
    check(message["type"] == "recorder/import_statistics", "message type")
    check(message["id"] == 7, "message id")
    check(message["metadata"]["statistic_id"] == "esp32ir:meter_energy", "statistic id")
    check(len(message["stats"]) == 1, "one stat bucket")


def main():
    test_parse_records_skips_bad_entries()
    test_aggregate_hourly_weighted_mean()
    test_aggregate_splits_hours_and_sorts()
    test_cumulative_energy_monotonic()
    test_metadata_old_api()
    test_metadata_new_api()
    test_build_stats_shapes()
    test_full_message_round_trip()
    print("\n%d checks, %d failures" % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
