# WP-Dashboard "WP Dashboard" (wp-native) — rein native HA-Karten

Neu angelegtes, eigenstaendiges Dashboard auf dem Odroid N2+ (HA 2026.10.2,
Supervised). Nutzt **nur native Home-Assistant-Karten** — kein Grafana, kein
ApexCharts, keine Custom Card. Bestehende Dashboards (`waermepumpe-dashboard`,
`wp-grafana`) bleiben unveraendert.

- URL-Pfad: `/wp-native` (Sidebar-Titel "WP Dashboard", Icon `mdi:heat-pump`)
- Config: `remote-shell/ha/wp_native_dashboard.json`
- Deploy: `python3 remote-shell/ha/deploy_wp_native_dashboard.py`
  (legt per WebSocket-API an; **kein HA-Neustart noetig**, idempotent)

## Aufbau (8 Sections)

1. Temperaturen — Gauges: Aussen, Vorlauf, Ruecklauf, Warmwasser, Raum, WP-Vorlauf
2. Leistung & Effizienz — Gauges: elektr. Leistung, therm. Leistung (1 h), COP (1 h), Modulation
3. Betrieb — `entities`: WP laeuft, Heizkreis-/Fussboden-/Warmwasser-Status, Druck, Volumenstrom
4. Energie heute — `statistic` (change, Kalendertag)
5. Energie kumuliert — `entities`
6. Verlaeufe (24 h) — zwei `history-graph`
7. Energieverlauf — `statistics-graph` (Balken, 9 Tage)
8. Netz (Stromzaehler) — Gauge + `entities` + `history-graph` + `statistics-graph` (MQTT-Stromzaehler)

## Verwendete Entity-IDs (alle real vorhanden, geprueft)

Temperaturen/Status (bosch_k40rf):
`sensor.mx_400_outdoor_temperature`, `sensor.mx_400_supply_temperature`,
`sensor.mx_400_return_temperature`, `sensor.warmwasser_actual_temperature`,
`sensor.mx_400_system_internal_room_temperature`,
`sensor.mx_400_hybman_temperature_flow_heat_pump`, `sensor.mx_400_system_pressure`,
`sensor.warmeerzeuger_2_pump_volume_flow`,
`sensor.heizkreis_1_current_operating_status`, `sensor.fussboden_overall_status`,
`sensor.warmwasser_overall_status`.

Leistung/Effizienz:
`sensor.mx_400_compressor_electrical_power`, `sensor.wp_waermeleistung_1h`,
`sensor.wp_cop_1h`, `sensor.wp_modulation`, `binary_sensor.wp_laeuft`.

Energie:
`sensor.warmeerzeuger_2_total_energy_compressor`,
`sensor.warmeerzeuger_2_total_energy_produced`,
`sensor.mx_400_system_heating_energy_electricity`,
`sensor.mx_400_system_hot_water_energy_electricity`, `sensor.wp_cop_gesamt`,
`sensor.warmeerzeuger_2_compressor_starts_total`,
`sensor.warmeerzeuger_2_working_time_total`.

Stromzaehler (MQTT):
`sensor.esp32_ir_stromzaehler_leistung`, `sensor.esp32_ir_stromzaehler_bezug`,
`sensor.esp32_ir_stromzaehler_einspeisung`,
`sensor.esp32_ir_stromzaehler_leistung_15_min_mittel`.

## Grenzwerte (Gauges)

Die Bereiche stammen aus realen Messwerten bzw. Geraetedaten, nicht aus
willkuerlichen Skalen:

- Aussen −20..40 °C, Vorlauf/Ruecklauf/Warmwasser 0..80 °C, Raum 5..30 °C
- elektr. Leistung 0..3000 W (Anschlusswert, vgl. `wp_modulation`-Template: 3000 W = 100 %)
- therm. Leistung 1 h 0..10 kW, COP 1 h 1..6, Modulation 0..100 %
- Netzleistung 0..6000 W

## Einschraenkungen (bewusst, dokumentiert)

- **Kein natives Thermostat.** `bosch_k40rf` liefert **keine `climate`-Entitaet**
  (geprueft), daher keine `thermostat`-Karte. Betriebszustaende werden als
  Status-Sensoren angezeigt.
- Heiz-/Warmwasserbetrieb ist nicht als sauberer Phasenverlauf verfuegbar
  (keine passenden Binaersensoren mit Statistik), daher Statusdarstellung.
- `statistics-graph` benoetigt Langzeitstatistiken; diese sind vorhanden
  (~15 h, 5-min-Raster). Nach einigen Tagen werden die Balken aussagekraeftiger.

## Wiederherstellung

- Dashboard entfernen: in HA **Einstellungen → Dashboards** "WP Dashboard"
  loeschen, oder per WS `lovelace/dashboards/delete {"url_path":"wp-native"}`.
- Datei-Backups: `.storage/lovelace.wp_native.bak-*` (bei jedem Deploy) sowie
  die Register-Backups `.storage/lovelace_dashboards.bak-*`.
- Die bestehenden Dashboards wurden nicht angefasst (per md5 gegen das
  Backup verifiziert).
