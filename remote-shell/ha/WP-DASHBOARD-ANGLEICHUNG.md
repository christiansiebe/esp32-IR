# waermepumpe-dashboard → Angleichung an die Buderus-Vorlage

Referenz: `bosch-buderus-wp/home-assistant` →
`dashboards/simple-dashboard.yaml`
(<https://github.com/bosch-buderus-wp/home-assistant/blob/main/dashboards/simple-dashboard.yaml>).

Dieses Dokument beschreibt, was am **aelteren** Dashboard
`waermepumpe-dashboard` (Storage `.storage/lovelace.waermepumpe_dashboard`,
gepflegt von `remote-shell/ha/ha_wp_dashboard.py`, JS
`remote-shell/ha/ha_wp_zoom_chart.js`) gegenueber der Vorlage fehlt und wie es
ergaenzt wird. Das **neuere** `wp-native` (PR #53 + Nachfolger) ist bereits
vollstaendig an der Vorlage ausgerichtet und separat in
`WP-NATIVE-DASHBOARD.md` dokumentiert.

## Ist-Stand (warum hier zwei Dashboards)

- `waermepumpe-dashboard` ist die **gewachsene** Ansicht: Zoom-Karten
  (`custom:wp-zoom-chart`) mit Klick-Vollbild, adaptiver Aufloesung und
  Zeitraeumen (1 h … 1 J) — das ist das **Plus** gegenueber der Vorlage, die nur
  `history-graph` und `sensor`-line-Karten kennt. Zusaetzlich HACS-ApexCharts
  (`custom:apexcharts-card`) und der abgeleitete Template-Sensor
  `sensor.wp_modulation`.
- `wp-native` ist die **rein native** Nachbildung der Vorlage ohne jede Custom
  Card.
- Die Vorlage bleibt die Struktur-Referenz; die Zoom-Karten bleiben das
  Mehr an Funktion, das `waermepumpe-dashboard` ausmacht.

## Was in `waermepumpe-dashboard` fehlte (Luecken zur Vorlage)

| Vorlage-Element | Status vorher | jetzt |
|---|---|---|
| `type: sections`, `max_columns: 4` | schon `sections` | ok |
| `column_span` je Section | teils gesetzt | ok (Bestand) |
| Kopf-`badges` (Modus) | fehlte | **ergaenzt** (`ensure_mode_badge`) |
| `Silent`-Badge (`select.*`) | fehlte | **bewusst ausgelassen** (read-only, kein `select`) |
| gruppierte "Energiebilanz"-Stacks (`vertical-stack`→`horizontal-stack`) | fehlte | **ergaenzt** (`ensure_energiebilanz_section`) |
| `statistic` (calendar/day, change) | ueber Zoom-Karten abgedeckt | Zoom-Karten bleiben (bewusst) |
| `gauge` mit `needle` + `grid_options` | ueber Zoom/Gauges vorhanden | ok |
| `history-graph` | ueber Zoom-Karten abgedeckt | Zoom-Karten bleiben (bewusst) |
| `statistics-graph` (bar) | ueber Zoom-Karten abgedeckt | Zoom-Karten bleiben (bewusst) |
| `thermostat`-Karte | fehlte | **bewusst ausgelassen** (kein `climate`) |

## Was `ha_wp_dashboard.py` jetzt idempotent ergaenzt

Zwei risikoarme, rein additive Struktur-Elemente (kein Entfernen, kein
Umschreiben bestehender Karten), beide host-getestet in
`test_ha_wp_dashboard.py`:

1. **`ensure_mode_badge(view)`** — haengt das Kopf-Badge **Modus**
   (`binary_sensor.wp_laeuft`) an, wenn es fehlt. Ein bereits vorhandenes,
   individuell gestyltes Badge fuer dieselbe Entitaet wird respektiert.
2. **`ensure_energiebilanz_section(view)`** — haengt eine Section
   "Energiebilanz" mit drei `vertical-stack`-Gruppen (Heizung, Warmwasser,
   Total) im Vorlage-Stil an, wenn die Section fehlt. Erkannt wird sie am
   Heading, daher kein Doppelanlegen.

Beide sind **idempotent** (zweiter Lauf ist ein No-op) und aendern nur, wenn
etwas fehlt. Bestehende Karten bleiben unberuehrt.

## Bewusst NICHT uebertragen

- **`thermostat`-Karte**: die Vorlage steuert `climate.thermostat_hc1`. Die
  Integration `bosch_k40rf` ist **read-only** und liefert hier keine
  `climate`-Entitaet.
- **`Silent`-Badge**: die Vorlage bindet `select.boiler_silentmode`. Es gibt
  hier **keine `select`-Entitaet** und kein `boiler_*`-Entity.
- **Zoom-Karten entfernen**: nicht gewuenscht — sie sind das funktionale Plus
  (Klick-Vollbild, adaptive Aufloesung, Zeitraeume), das die Vorlage nicht hat.

## Folgeschritte (falls gewuenscht)

- Das `statistic`/`statistics-graph`-Verhalten der Vorlage (Langzeitstatistiken
  statt Zoom-Karten) liesse sich zusaetzlich als eigene Section ergaenzen; die
  Zoom-Karten decken denselben Nutzen bereits ab, daher hier nicht noetig.
- Ein Zusammenfuehren von `waermepumpe-dashboard` und `wp-native` ist **nicht**
  vorgesehen: das eine ist die Custom-Card-Ansicht, das andere die rein native.
