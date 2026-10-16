"""Host tests for remote-shell/ha/ha_wp_dashboard.py (pure logic, no network).

Only ``resource_url`` is exercised: the deploy itself needs a live HA and is
covered by the end-to-end run on the Odroid. ``jwt``/``websockets`` are stubbed
so the module imports on a plain host.

Why this matters: HA serves ``/local/*`` with ``Cache-Control: max-age=2678400``
(31 days). A Lovelace resource URL without a query string is therefore cached
for a month and every card update silently never reaches the browser. The
content hash makes each deploy a new URL.
"""

import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

for name in ("jwt", "websockets"):
    sys.modules.setdefault(name, types.ModuleType(name))

import ha_wp_dashboard as hwd  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def write(path, data):
    with open(path, "wb") as fh:
        fh.write(data)


def test_url_is_versioned_and_content_addressed(tmp="/tmp/hwd_card.js"):
    print("test_url_is_versioned_and_content_addressed")
    write(tmp, b"console.log(1)")
    a = hwd.resource_url(tmp)
    check(a.startswith("/local/wp-zoom-chart.js?v="), f"prefix: {a}")
    ver = a.split("?v=")[1]
    check(len(ver) == 10, f"hash length: {ver}")

    # Same bytes -> same URL (idempotent re-deploy, no cache churn).
    check(hwd.resource_url(tmp) == a, "stable for identical content")

    # Changed bytes -> changed URL (busts the browser cache).
    write(tmp, b"console.log(2)")
    b = hwd.resource_url(tmp)
    check(b != a, f"hash must change with content: {a} vs {b}")
    os.remove(tmp)


def test_apex_card_becomes_resizable():
    print("test_apex_card_becomes_resizable")
    card = {
        "type": "custom:apexcharts-card",
        "apex_config": {"chart": {"height": 380}, "grid": {"borderColor": "#dddddd"}},
    }
    check(hwd.normalize_apex_card(card) is True, "changed on first pass")
    check(card["section_mode"] is True, "section_mode enabled")
    check("height" not in card["apex_config"]["chart"], "fixed height removed")
    check(card["apex_config"]["grid"]["borderColor"] == "#dddddd", "rest untouched")
    # Idempotent: a second pass reports no change.
    check(hwd.normalize_apex_card(card) is False, "idempotent on second pass")


def test_other_cards_untouched():
    print("test_other_cards_untouched")
    card = {"type": "custom:wp-zoom-chart", "title": "x"}
    check(hwd.normalize_apex_card(card) is False, "not an apex card -> no change")
    check(card == {"type": "custom:wp-zoom-chart", "title": "x"}, "unchanged")


def test_recurses_into_stacks_and_sections():
    print("test_recurses_into_stacks_and_sections")
    cfg = {"views": [{"sections": [
        {"cards": [
            {"type": "vertical-stack", "cards": [
                {"type": "custom:apexcharts-card", "apex_config": {"chart": {"height": 300}}},
            ]},
            {"type": "custom:apexcharts-card", "apex_config": {}},
        ]},
    ]}]}
    check(hwd.normalize_cards(cfg) is True, "found nested cards")
    nested = cfg["views"][0]["sections"][0]["cards"][0]["cards"][0]
    direct = cfg["views"][0]["sections"][0]["cards"][1]
    check(nested["section_mode"] is True, "nested card normalized")
    check(direct["section_mode"] is True, "direct card normalized")


def test_wrap_expand_card_is_idempotent():
    print("test_wrap_expand_card_is_idempotent")
    inner = {"type": "custom:apexcharts-card", "section_mode": True}
    wrapped = hwd.wrap_expand_card(inner, "Energiemonitoring")
    check(wrapped["type"] == "custom:wp-expand-card", "wrapper type")
    check(wrapped["title"] == "Energiemonitoring", "title carried")
    check(wrapped["card"] == inner, "inner card preserved")
    # Wrapping again must not nest a second time.
    check(hwd.wrap_expand_card(wrapped, "x") is wrapped, "idempotent")


def test_wrap_all_apex_only_touches_apex():
    print("test_wrap_all_apex_only_touches_apex")
    cfg = {"views": [{"sections": [{"cards": [
        {"type": "custom:apexcharts-card"},
        {"type": "custom:wp-zoom-chart", "title": "Verlauf"},
        {"type": "sensor"},
    ]}]}]}
    n = hwd.wrap_all_apex(cfg)
    check(n == 1, f"one apex card wrapped, got {n}")
    cards = cfg["views"][0]["sections"][0]["cards"]
    check(cards[0]["type"] == "custom:wp-expand-card", "apex wrapped")
    check(cards[0]["card"]["type"] == "custom:apexcharts-card", "inner apex kept")
    check(cards[1]["type"] == "custom:wp-zoom-chart", "zoom card untouched")
    check(cards[2]["type"] == "sensor", "plain card untouched")


if __name__ == "__main__":
    test_url_is_versioned_and_content_addressed()
    test_apex_card_becomes_resizable()
    test_other_cards_untouched()
    test_recurses_into_stacks_and_sections()
    test_wrap_expand_card_is_idempotent()
    test_wrap_all_apex_only_touches_apex()
    print(f"\n{CHECKS - FAILURES}/{CHECKS} checks passed")
    sys.exit(1 if FAILURES else 0)
