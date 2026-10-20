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

def test_reference_gauges_idempotent():
    print("test_reference_gauges_idempotent")
    view = {"sections": [{"cards": [
        {"type": "gauge", "entity": "sensor.mx_400_compressor_electrical_power"},
        {"type": "gauge", "entity": "sensor.wp_cop_1h"},
    ]}]}
    check(hwd.ensure_reference_gauges(view), "first run changes")
    ents = [c["entity"] for c in view["sections"][0]["cards"]]
    check("sensor.buderus_thermische_leistung_heizung" in ents, "thermal gauge added")
    check(hwd.MOD_ENTITY in ents, "modulation gauge added")
    before = list(ents)
    check(not hwd.ensure_reference_gauges(view), "second run is a no-op")
    after = [c["entity"] for c in view["sections"][0]["cards"]]
    check(before == after, "no duplicates on rerun")


def test_replace_raw_modulation_tile():
    print("test_replace_raw_modulation_tile")
    view = {"sections": [{"cards": [
        {"type": "sensor", "entity": "sensor.mx_400_actual_modulation"},
    ]}]}
    check(hwd.replace_raw_modulation_tile(view), "tile changed")
    c = view["sections"][0]["cards"][0]
    check(c["entity"] == hwd.MOD_ENTITY, "points at derived sensor")
    check(not hwd.replace_raw_modulation_tile(view), "idempotent")




def test_upsert_marker_block_is_idempotent():
    print("test_upsert_marker_block_is_idempotent")
    s, e = "# >>> X <<<", "# <<< X <<<"
    block = s + "\nbody\n" + e + "\n"
    t1 = hwd.upsert_marker_block("pre\n", s, e, block)
    check("body" in t1 and t1.startswith("pre"), "append works")
    t2 = hwd.upsert_marker_block(t1, s, e, block)
    check(t2 == t1, "idempotent")
    check(t2.count(s) == 1 and t2.count(e) == 1, "no duplicate markers")
    t3 = hwd.upsert_marker_block(t1, s, e, s + "\nNEW\n" + e + "\n")
    check("NEW" in t3 and "body" not in t3, "replace body")
    check(t3.count(s) == 1 and t3.count(e) == 1, "still one marker pair")


def test_ensure_mode_badge_idempotent():
    print("test_ensure_mode_badge_idempotent")
    view = {}
    check(hwd.ensure_mode_badge(view), "first run adds the badge")
    check(view["badges"][0]["entity"] == hwd.BADGE_MODE_ENTITY, "badge entity")
    check(view["badges"][0]["name"] == "Modus", "badge name")
    before = list(view["badges"])
    check(not hwd.ensure_mode_badge(view), "second run is a no-op")
    check(view["badges"] == before, "no duplicate badge")
    # An existing (user-styled) badge for the same entity is respected.
    view2 = {"badges": [{"type": "entity", "entity": hwd.BADGE_MODE_ENTITY,
                         "name": "Mein Modus"}]}
    check(not hwd.ensure_mode_badge(view2), "existing badge respected")
    check(view2["badges"][0]["name"] == "Mein Modus", "styling untouched")


def test_ensure_energiebilanz_section_idempotent():
    print("test_ensure_energiebilanz_section_idempotent")
    view = {"sections": [{"type": "grid", "cards": [
        {"type": "heading", "heading": "Temperaturen"}]}]}
    check(hwd.ensure_energiebilanz_section(view), "first run adds the section")
    added = view["sections"][-1]
    check(added["type"] == "grid" and added["column_span"] == 3, "grid + span")
    check(added["cards"][0].get("heading") == "Energiebilanz", "section heading")
    types = [c["type"] for c in added["cards"][1:]]
    check(types == ["vertical-stack"] * 3, f"three stacks: {types}")
    for stack in added["cards"][1:]:
        check(stack["cards"][1]["type"] == "horizontal-stack", "stack has h-row")
    n = len(view["sections"])
    check(not hwd.ensure_energiebilanz_section(view), "second run is a no-op")
    check(len(view["sections"]) == n, "no duplicate section")


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
    test_reference_gauges_idempotent()
    test_replace_raw_modulation_tile()
    test_upsert_marker_block_is_idempotent()
    test_ensure_mode_badge_idempotent()
    test_ensure_energiebilanz_section_idempotent()
    print(f"\n{CHECKS - FAILURES}/{CHECKS} checks passed")
    sys.exit(1 if FAILURES else 0)
