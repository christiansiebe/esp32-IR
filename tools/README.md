# Home Assistant import tools

The receiver stores up to ~20 days of minute averages in its SPIFFS `storage`
partition. Home Assistant does **not** pull history over MQTT: its recorder only
records what is published live. To get the already-stored records into Home
Assistant you push them in with the `recorder/import_statistics` WebSocket
command. There is no REST endpoint for that, so `ha_import.py` bridges the two.

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
