"""Host tests for tools/ha_gateway_history.py (pure logic, no network).

Only ``build_points`` is exercised here: the gateway fetch and the WebSocket
import need a live HA + gateway and are covered by the end-to-end run on the
Odroid, not by CI.
"""

import datetime as dt
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import ha_gateway_history as hgh  # noqa: E402

CHECKS = 0
FAILURES = 0

UTC = dt.timezone.utc


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def d(day):
    return dt.datetime(2025, 1, day, tzinfo=UTC)


def test_only_pre_cutoff_days_are_kept():
    print("test_only_pre_cutoff_days_are_kept")
    daily = {d(1): 10.0, d(2): 11.0, d(3): 12.0, d(4): 13.0}
    cutoff = d(3).timestamp()
    pts = hgh.build_points(daily, {}, cutoff)
    starts = [p["start"][:10] for p in pts]
    check(starts == ["2025-01-01", "2025-01-02"], f"only days before cutoff: {starts}")
    check(all(p["mean"] in (10.0, 11.0) for p in pts), "means preserved")


def test_day_equal_to_cutoff_is_dropped():
    print("test_day_equal_to_cutoff_is_dropped")
    daily = {d(2): 11.0, d(3): 12.0}
    pts = hgh.build_points(daily, {}, d(3).timestamp())
    check(len(pts) == 1 and pts[0]["start"][:10] == "2025-01-02",
          "the cutoff day itself must not be imported (no overlap)")


def test_no_cutoff_imports_all_days():
    print("test_no_cutoff_imports_all_days")
    daily = {d(1): 1.0, d(2): 2.0}
    pts = hgh.build_points(daily, {}, None)
    check(len(pts) == 2, "no HA sample yet -> import everything")


def test_months_fill_only_before_first_day():
    print("test_months_fill_only_before_first_day")
    daily = {dt.datetime(2025, 6, 15, tzinfo=UTC): 20.0}
    monthly = {dt.datetime(2025, 3, 1, tzinfo=UTC): 5.0,
               dt.datetime(2025, 4, 1, tzinfo=UTC): 6.0,
               dt.datetime(2025, 6, 1, tzinfo=UTC): 7.0}  # same month as daily -> drop
    pts = hgh.build_points(daily, monthly, None)
    starts = sorted(p["start"][:7] for p in pts)
    check(starts == ["2025-03", "2025-04", "2025-06"],
          f"months before first daily kept, same-month dropped: {starts}")


def test_months_imported_without_daily_anchor():
    print("test_months_imported_without_daily_anchor")
    # No daily buckets at all: months are still useful (e.g. a monthly-only resource).
    pts = hgh.build_points({}, {d(1): 1.0}, None)
    check(len(pts) == 1 and pts[0]["start"][:7] == "2025-01",
          "monthly-only data is imported when no daily anchor exists")


def test_sorted_and_round_tripped():
    print("test_sorted_and_round_tripped")
    daily = {d(3): 3.0, d(1): 1.0, d(2): 2.0}
    pts = hgh.build_points(daily, {}, None)
    check([p["mean"] for p in pts] == [1.0, 2.0, 3.0], "chronological order")
    check(all(isinstance(p["start"], str) for p in pts), "ISO strings")


def test_empty_inputs():
    print("test_empty_inputs")
    check(hgh.build_points({}, {}, None) == [], "no data -> no points")


def main():
    for fn in (test_only_pre_cutoff_days_are_kept,
               test_day_equal_to_cutoff_is_dropped,
               test_no_cutoff_imports_all_days,
               test_months_fill_only_before_first_day,
               test_months_imported_without_daily_anchor,
               test_sorted_and_round_tripped,
               test_empty_inputs):
        fn()
    print(f"\n{CHECKS} checks, {FAILURES} failures")
    sys.exit(1 if FAILURES else 0)


if __name__ == "__main__":
    main()
