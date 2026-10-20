#!/usr/bin/env python3
"""Structural guard for ``wp_native_dashboard.json`` (pure, no HA needed).

The dashboard is a plain Lovelace config consumed by HA's frontend, so nothing
in the firmware build would notice a broken card type, a stray entity ID or a
view that silently lost its ``sections`` type. This test reads the committed
JSON and asserts the shape the Buderus-derived layout relies on:

  1. valid JSON with exactly one view,
  2. ``type: sections`` and ``max_columns: 4`` (the reference template),
  3. every section is a ``grid`` with a ``column_span``,
  4. every card type is a known native Lovelace card (no custom: cards),
  5. every referenced entity ID belongs to the whitelist of entities that
     really exist on this install - and none is a ``boiler_*`` entity (that
     naming is from the read-only Bosch reference, not from this project),
  6. the reference-only elements we deliberately dropped are absent
     (``thermostat`` card, ``select.*silentmode`` badge).

Run directly:

    python3 remote-shell/ha/test_wp_native_dashboard.py

It is intentionally NOT wired into the firmware ``test/run_host_tests.sh``
(that suite is the ESP-IDF host build); run it on its own.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(HERE, "wp_native_dashboard.json")

# Native Lovelace card types that may appear in this dashboard.
NATIVE_CARD_TYPES = {
    "heading", "sensor", "gauge", "statistic", "entity", "entities",
    "history-graph", "statistics-graph", "vertical-stack", "horizontal-stack",
}

# Entities that exist on the install (verified against /api/states). Any ID not
# in this set means someone invented an entity.
ALLOWED_ENTITIES = {
    "binary_sensor.wp_laeuft",
    "sensor.esp32_ir_stromzaehler_bezug",
    "sensor.esp32_ir_stromzaehler_einspeisung",
    "sensor.esp32_ir_stromzaehler_leistung",
    "sensor.esp32_ir_stromzaehler_leistung_15_min_mittel",
    "sensor.fussboden_overall_status",
    "sensor.heizkreis_1_current_operating_status",
    "sensor.mx_400_compressor_electrical_power",
    "sensor.mx_400_hybman_temperature_flow_heat_pump",
    "sensor.mx_400_outdoor_temperature",
    "sensor.mx_400_return_temperature",
    "sensor.mx_400_supply_temperature",
    "sensor.mx_400_system_heating_energy_electricity",
    "sensor.mx_400_system_hot_water_energy_electricity",
    "sensor.mx_400_system_internal_room_temperature",
    "sensor.mx_400_system_pressure",
    "sensor.warmeerzeuger_2_compressor_starts_total",
    "sensor.warmeerzeuger_2_pump_volume_flow",
    "sensor.warmeerzeuger_2_total_energy_compressor",
    "sensor.warmeerzeuger_2_total_energy_produced",
    "sensor.warmeerzeuger_2_working_time_total",
    "sensor.warmwasser_actual_temperature",
    "sensor.warmwasser_overall_status",
    "sensor.wp_cop_1h",
    "sensor.wp_cop_gesamt",
    "sensor.wp_modulation",
    "sensor.wp_waermeleistung_1h",
}

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def load():
    with open(CONFIG, encoding="utf-8") as fh:
        return json.load(fh)


def iter_cards(node):
    """Yield every card dict, following only ``cards`` lists.

    Cards live in ``cards`` arrays (a section, a stack). Walking ``type`` keys
    generically would also match the view (``type: sections``) and the sections
    (``type: grid``), which are not cards.
    """
    if isinstance(node, dict):
        cards = node.get("cards")
        if isinstance(cards, list):
            for card in cards:
                if isinstance(card, dict):
                    yield card
        for value in node.values():
            if isinstance(value, (dict, list)):
                yield from iter_cards(value)
    elif isinstance(node, list):
        for child in node:
            yield from iter_cards(child)


def iter_entities(node):
    """Yield every entity ID referenced anywhere in the config."""
    if isinstance(node, dict):
        ent = node.get("entity")
        if isinstance(ent, str) and "." in ent:
            yield ent
        entities = node.get("entities")
        if isinstance(entities, list):
            for item in entities:
                if isinstance(item, str) and "." in item:
                    yield item
                elif isinstance(item, dict) and isinstance(item.get("entity"), str):
                    yield item["entity"]
        for value in node.values():
            if isinstance(value, (dict, list)):
                yield from iter_entities(value)
    elif isinstance(node, list):
        for child in node:
            yield from iter_entities(child)


def test_valid_json_single_view():
    print("test_valid_json_single_view")
    cfg = load()
    check("views" in cfg, "config has a 'views' key")
    check(len(cfg["views"]) == 1, f"exactly one view: {len(cfg['views'])}")


def test_view_is_sections_with_four_columns():
    print("test_view_is_sections_with_four_columns")
    view = load()["views"][0]
    check(view.get("type") == "sections", f"type sections: {view.get('type')}")
    check(view.get("max_columns") == 4, f"max_columns 4: {view.get('max_columns')}")
    check(bool(view.get("title")), "view has a title")
    check(isinstance(view.get("sections"), list) and view["sections"],
          "view has sections")


def test_every_section_is_grid_with_column_span():
    print("test_every_section_is_grid_with_column_span")
    view = load()["views"][0]
    for i, section in enumerate(view["sections"]):
        check(section.get("type") == "grid", f"section {i} is a grid")
        check(isinstance(section.get("column_span"), int),
              f"section {i} has an int column_span")
        check(bool(section.get("cards")), f"section {i} has cards")


def test_every_card_is_native():
    print("test_every_card_is_native")
    for card in iter_cards(load()):
        ctype = card["type"]
        check(ctype in NATIVE_CARD_TYPES, f"native card type: {ctype}")


def test_referenced_entities_are_allowed():
    print("test_referenced_entities_are_allowed")
    used = set(iter_entities(load()))
    check(bool(used), "config references at least one entity")
    for ent in sorted(used):
        check(ent in ALLOWED_ENTITIES, f"entity is on the whitelist: {ent}")
    boiler = sorted(e for e in used if e.startswith("boiler_"))
    check(not boiler, f"no boiler_* entity (reference-only): {boiler}")


def test_reference_only_elements_are_dropped():
    print("test_reference_only_elements_are_dropped")
    cfg = load()
    view = cfg["views"][0]
    # No thermostat card: bosch_k40rf exposes no climate entity.
    check(not any(c["type"] == "thermostat" for c in iter_cards(cfg)),
          "no thermostat card")
    # No select.* entity anywhere (the Silent badge cannot exist here).
    selects = [e for e in iter_entities(cfg) if e.startswith("select.")]
    check(not selects, f"no select.* entity: {selects}")
    # The mode badge is the WP running binary sensor, not a silent-mode select.
    badges = view.get("badges", [])
    check(any(b.get("entity") == "binary_sensor.wp_laeuft" for b in badges),
          "Modus badge maps to binary_sensor.wp_laeuft")


def test_reference_layout_elements_present():
    print("test_reference_layout_elements_present")
    cfg = load()
    types = {c["type"] for c in iter_cards(cfg)}
    for wanted in ("sensor", "gauge", "statistic", "history-graph",
                   "statistics-graph", "vertical-stack", "horizontal-stack",
                   "heading"):
        check(wanted in types, f"reference element present: {wanted}")
    # sensor line cards carry graph: line + detail: 1
    lines = [c for c in iter_cards(cfg) if c["type"] == "sensor"]
    check(lines, "there is at least one sensor card")
    for c in lines:
        check(c.get("graph") == "line", "sensor card graph: line")
        check(c.get("detail") == 1, "sensor card detail: 1")
    # gauges are needle gauges with grid_options
    gauges = [c for c in iter_cards(cfg) if c["type"] == "gauge"]
    check(gauges, "there is at least one gauge")
    for c in gauges:
        check(c.get("needle") is True, "gauge needle: true")
        check(isinstance(c.get("grid_options"), dict), "gauge has grid_options")
    # statistics are calendar/day change
    stats = [c for c in iter_cards(cfg) if c["type"] == "statistic"]
    check(stats, "there is at least one statistic card")
    for c in stats:
        check(c.get("stat_type") == "change", "statistic stat_type: change")
        check(c.get("period") == {"calendar": {"period": "day"}},
              "statistic period: calendar/day")
    # statistics-graph is a bar chart
    graphs = [c for c in iter_cards(cfg) if c["type"] == "statistics-graph"]
    check(graphs, "there is at least one statistics-graph")
    for c in graphs:
        check(c.get("chart_type") == "bar", "statistics-graph chart_type: bar")


def main():
    for test in (
        test_valid_json_single_view,
        test_view_is_sections_with_four_columns,
        test_every_section_is_grid_with_column_span,
        test_every_card_is_native,
        test_referenced_entities_are_allowed,
        test_reference_only_elements_are_dropped,
        test_reference_layout_elements_present,
    ):
        test()
    print(f"\n{CHECKS} checks, {FAILURES} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
