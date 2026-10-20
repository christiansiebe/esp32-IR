# WP-Dashboard "WP Dashboard" (wp-native) — native HA-Karten, Buderus-Aufbau

Eigenstaendiges Dashboard auf dem Odroid N2+ (HA 2026.10.2, Supervised). Nutzt
**nur native Home-Assistant-Karten** — kein Grafana, kein ApexCharts, keine
Custom Card. Aufbau und Karten-Gruppierung folgen der Vorlage
`bosch-buderus-wp/home-assistant` → `dashboards/simple-dashboard.yaml`.
Bestehende Dashboards (`waermepumpe-dashboard`, `wp-grafana`) bleiben
unveraendert.

- URL-Pfad: `/wp-native` (Sidebar-Titel "WP Dashboard", Icon `mdi:heat-pump`)
- Config: `remote-shell/ha/wp_native_dashboard.json`
- Deploy: `python3 remote-shell/ha/deploy_wp_native_dashboard.py`
  (legt per WebSocket-API an; **kein HA-Neustart noetig**, idempotent)
- Struktur-Test: `python3 remote-shell/ha/test_wp_native_dashboard.py`

## Aufbau (1 View "Ueberblick", `type: sections`, `max_columns: 4`)

Die Vorlage hat genau **eine** View und setzt je Section `column_span` (4er-
Raster). Das wird uebernommen: eine View, fuenf Sections, jede mit
`column_span: 4`.

1. **Aktuelle Werte** — `sensor`-Karten mit `graph: line` + `detail: 1`
   (Aussen, Vorlauf, Warmwasser, Ruecklauf, WP-Vorlauf, Raum, Modulation),
   darunter `gauge`-Karten mit `needle: true` und `grid_options`
   (elektr. Leistung, therm. Leistung 1 h, COP 1 h). Vorlage: erstes
   "Aktuelle Werte"-Grid mit `sensor`-line-Karten und Needle-Gauges.
2. **Heute** — `history-graph` (WP laeuft, 24 h), zwei gruppierte
   "Energiebilanz"-Stacks (`vertical-stack` → `horizontal-stack` mit
   `statistic`, `period: calendar/day`, `stat_type: change`) fuer Heizung und
   Warmwasser, plus `statistics-graph` (`chart_type: bar`, `period: hour`,
   `days_to_show: 1`). Vorlage: "Heute"-Grid.
3. **Gesamte Laufzeit** — gruppierte "Energiebilanz"-Stacks als
   `entity`-Karten (Heizung, Warmwasser, Total, Laufzeit). Vorlage: "Gesamte
   Laufzeit"-Grid mit `vertical-stack`/`horizontal-stack`.
4. **Betrieb** — `entities`-Karte mit den Status-Sensoren (WP laeuft,
   Heizkreis/Fussboden/Warmwasser-Status, Druck, Volumenstrom). Aus dem
   vorherigen wp-native uebernommen; die Vorlage hat hier keinen direkten
   Gegenpart.
5. **Netz (Stromzaehler)** — Gauge + `entities` + `statistics-graph` (bar) fuer
   den ESP32-IR-MQTT-Stromzaehler. Zusatz gegenueber der Vorlage (sie kennt nur
   die Waermepumpe).

Kopf-Badge: **Modus** (`entity`-Badge auf `binary_sensor.wp_laeuft`), wie in der
Vorlage der `sensor.boiler_hpactivity`-Badge.

## Uebernommene Referenz-Elemente

| Vorlage | hier |
|---|---|
| eine View, `type: sections`, `max_columns: 4` | uebernommen |
| `column_span` je Section | uebernommen (`4`) |
| `sensor`-Karten `graph: line` + `detail: 1` | uebernommen (7 Karten) |
| `gauge` mit `needle: true` + `grid_options` | uebernommen |
| `vertical-stack`/`horizontal-stack`-Gruppen | uebernommen (Energiebilanz-Stacks) |
| `statistic` `period: calendar/day`, `stat_type: change` | uebernommen |
| `history-graph` | uebernommen |
| `statistics-graph` `chart_type: bar` | uebernommen |
| `entity`-Badge (Modus) | uebernommen, auf `binary_sensor.wp_laeuft` |

## Bewusst ausgelassene Referenz-Elemente (und warum)

- **`thermostat`-Karte** — die Vorlage steuert `climate.thermostat_hc1`. Die
  Integration `bosch_k40rf` ist **read-only** und liefert hier **keine
  `climate`-Entitaet** (geprueft). Eine Thermostat-Karte gaebe es also nur mit
  einer nicht vorhandenen Entitaet; Betriebszustaende erscheinen stattdessen als
  Status-Sensoren in Section "Betrieb".
- **`Silent`-Badge** — die Vorlage bindet `select.boiler_silentmode`. Hier
  existiert **keine `select`-Entitaet** (read-only), und es gibt kein
  `boiler_*`-Entity. Das Badge faellt weg; das **Modus**-Badge bleibt und zeigt
  `binary_sensor.wp_laeuft` (Betriebsstatus).
- **`boiler_*`-Entitaeten** — Namensraum der Referenz-Integration. Hier wird
  alles auf `mx_400_*`, `warmeerzeuger_2_*`, `warmwasser_*`, `wp_*` und
  `esp32_ir_stromzaehler_*` abgebildet.

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

- elektr. Leistung 0..3000 W (Anschlusswert, vgl. `wp_modulation`-Template:
  3000 W = 100 %)
- therm. Leistung 1 h 0..10 kW, COP 1 h 1..6
- Netzleistung 0..6000 W

## Einschraenkungen (bewusst, dokumentiert)

- **Kein natives Thermostat** (siehe oben): `bosch_k40rf` liefert keine
  `climate`-Entitaet, daher keine `thermostat`-Karte.
- Heiz-/Warmwasserbetrieb ist nicht als sauberer Phasenverlauf verfuegbar
  (keine passenden Binaersensoren mit Statistik), daher Statusdarstellung.
- `statistics-graph` benoetigt Langzeitstatistiken; diese sind vorhanden.
  Nach einigen Tagen werden die Balken aussagekraeftiger.
- Die Vorlage fasst je Energiebilanz **Verbrauch und Abgabe** in einen
  `horizontal-stack`. Hier gibt es nur *eine* elektrische Energiequelle je
  Zweig (Heizung/Warmwasser), daher steht dort je Stack eine Karte; die
  Gesamt-Abgabe liegt im "Total"-Stack (Section 3).

## Wiederherstellung

- Dashboard entfernen: in HA **Einstellungen → Dashboards** "WP Dashboard"
  loeschen, oder per WS `lovelace/dashboards/delete {"url_path":"wp-native"}`.
- Datei-Backups: `.storage/lovelace.wp_native.bak-*` (bei jedem Deploy) sowie
  die Register-Backups `.storage/lovelace_dashboards.bak-*`.
- Die bestehenden Dashboards werden nicht angefasst (per md5 gegen das
  Backup verifizierbar).
