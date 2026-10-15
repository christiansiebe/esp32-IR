# Home Assistant tools (heat pump dashboard)

Helpers for the Odroid's Home Assistant that are **not** part of the ESP32
firmware, so they live here under `remote-shell/` instead of in the firmware
tree (`main/`, `components/`, `tools/`, `test/`). The CI path filter
`paths-ignore: remote-shell/**` keeps them out of the firmware build.

## Files
- `ha_gateway_history.py` — backfill the heat-pump gateway's own daily/monthly
  recordings into HA long-term statistics for the time before the integration
  existed (the gateway keeps ~13 months, HA only records from setup).
- `ha_wp_zoom_chart.js` — custom Lovelace card: click-to-fullscreen history
  chart with a draggable border, adaptive resolution and time ranges
  (1h … 1J); raw history for short ranges, statistics for long ones.
- `ha_wp_dashboard.py` — deploy the card to HA's `config/www/`, register the
  Lovelace resource (idempotent) and add the zoom cards to the
  `waermepumpe-dashboard` if missing.

All three run **inside** the Home Assistant container so they can read
`/config/.storage` (gateway credentials, admin token) and the recorder DB.

## Gateway backfill

HA only records from the day the `bosch_k40rf` integration was configured, so
everything before that is missing from the database even though the gateway
still has it. `ha_gateway_history.py` pulls the recordings and imports the
buckets that precede HA's own first sample via the WebSocket command
`recorder/import_statistics` (there is no REST endpoint for it). Only buckets
older than HA's first stored point are imported, so live data is never
overwritten; re-running is idempotent.

```bash
docker exec homeassistant python3 /config/ha_gateway_history.py --dry-run
docker exec homeassistant python3 /config/ha_gateway_history.py
```

## Deploy the dashboard card

```bash
cp ha_wp_zoom_chart.js /usr/share/hassio/homeassistant/wp-zoom-chart.js
docker exec homeassistant python3 /config/ha_wp_dashboard.py
```

## Tests

The pure logic is covered without mocks (no network, no HA needed):

```bash
python3 remote-shell/ha/test_ha_gateway_history.py
python3 remote-shell/ha/check_wp_zoom_chart.py   # needs node
```
