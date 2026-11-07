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

## Measuring the board's own power draw (FNIRSI FNB-48)

`fnb48_read.py` reads a FNIRSI FNB-48 USB power meter through `/dev/hidrawN`
so you can measure what an ESP32 board really draws on the bench (e.g. the C6
receiver). Stdlib only - no `pyusb`, no `hidapi`, no udev rules.

```bash
# 60 s average, plus a per-sample CSV log
python3 tools/fnb48_read.py -n 60 -o c6.csv

# show the load bursts (spikes) above 0.15 W and their spacing
python3 tools/fnb48_read.py -n 30 --burst 0.15
```

The meter samples at a fixed 100 S/s. Averaging over a short window hides
short spikes, so log every sample (`-o`) and let `--burst` group them. This is
how the C6's periodic radio wake shows up: ~65 mW idle, a ~130 ms burst to
~240 mW every 5 s (the receiver's radio-save window), ~70 mW average.

If `/dev/hidraw0` is missing although the meter is plugged in, `usbhid`
sometimes fails to bind after a hub/plug event:

```bash
echo 1-1.1:1.0 > /sys/bus/usb/drivers/usbhid/bind
```

## Analysing a power-history CSV

`analyze_power_csv.py` checks a downloaded `/api/power-history.csv` for missing
minutes (the "no data loss" requirement). It reports time gaps (real loss),
sequence anomalies (forward loss vs. backward reorder), duplicate minutes and
minutes with an unusually low `sample_count`:

```bash
python3 tools/analyze_power_csv.py power-history.csv
```

