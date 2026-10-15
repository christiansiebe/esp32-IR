# Home Assistant import tools

The receiver stores up to ~20 days of minute averages in its SPIFFS `storage`
partition. Home Assistant does **not** pull history over MQTT: its recorder only
records what is published live. To get the already-stored records into Home
Assistant you push them in with the `recorder/import_statistics` WebSocket
command. There is no REST endpoint for that, so `ha_import.py` bridges the two.

## Files
- `ha_import.py` / `ha_stats.py` / `ws_min.py` — push the receiver's minute
  history into HA long-term statistics (hourly aggregation, WebSocket import).
- `ha_gateway_history.py` — backfill a heat-pump gateway's own recordings
  (daily/monthly buckets) into HA, for the time before the integration existed.
- `ha_wp_zoom_chart.js` — custom Lovelace card (click-to-zoom history chart with
  a draggable border, adaptive resolution and selectable time ranges); deploy to
  HA's `config/www/` and register it as a Lovelace resource.

## Quick start

1. In Home Assistant create a long-lived access token
   (Profile -> Security -> Long-lived access tokens).
2. Make sure Home Assistant can reach the device, and you can reach both the
   device and Home Assistant.
3. Run:

   ```bash
   ESP32IR_URL=http://192.168.4.1 \
   HA_URL=http://homeassistant.local:8123 \
   HA_TOKEN=<token> \
   python3 tools/ha_import.py --statistic-id esp32ir:meter_energy --unit kWh
   ```

   Add `--power-too` to also import an hourly mean power statistic (`W`), or
   `--dry-run` to print the payload without sending it.

Because the `statistic_id` uses a `:` (`esp32ir:meter_energy`), Home Assistant
treats it as an *external* statistic, which is exactly what the energy
dashboard's "add external statistics" flow expects.

## How it works

- `ha_import.py` pages through `/api/power-history?after=<sequence>` to collect
  every stored minute record.
- `ha_stats.py` aggregates those minutes into hourly buckets (weighted by
  sample count) and builds the import payload, adapting to the HA statistics
  API version (HA 2025.10 replaced `has_mean` with `mean_type` and added
  `unit_class`).
- `ws_min.py` is a small stdlib-only WebSocket client, so no extra dependency
  is needed.

## Tests

The pure logic and the framing are covered without mocks:

```bash
python3 test/test_ha_stats.py
python3 test/test_ws_min.py
python3 test/test_ws_client.py   # runs ws_min.Client against a real local server
```

## Analysing a power-history CSV

`analyze_power_csv.py` checks a downloaded `/api/power-history.csv` for missing
minutes (the "no data loss" requirement). It reports time gaps (real loss),
sequence anomalies (forward loss vs. backward reorder), duplicate minutes and
minutes with an unusually low `sample_count`:

```bash
python3 tools/analyze_power_csv.py power-history.csv
```

## Backfilling a heat-pump gateway's own history

A Bosch/Buderus gateway (K40RF local API) keeps its own long-term history as
daily/monthly recordings that reach far beyond HA's recorder. HA only records
from the day the `bosch_k40rf` integration was set up, so everything before that
is missing from the HA database even though the gateway still has it.

`ha_gateway_history.py` pulls those recordings and imports the buckets that
precede HA's own first sample into HA long-term statistics (via the same
`recorder/import_statistics` WebSocket command). It runs **inside** the HA
container so it can read the gateway credentials and mint an admin token from
HA's `.storage`:

```bash
docker exec homeassistant python3 /config/ha_gateway_history.py --dry-run
docker exec homeassistant python3 /config/ha_gateway_history.py
```

Only buckets older than HA's first stored point are imported, so live data is
never overwritten; re-running is idempotent. The pure selection logic is covered
by `test/test_ha_gateway_history.py`.

