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

## Odroid CPU tools (outside the HA container)

- `cpu-volt-watch`, `cpu-volt-line`, `htopv`, `htopv.tmux.conf`,
  `deploy_htopv.sh`, `test_htopv.py` + `htopv-README.md` — live per-core MHz
  and per-cluster voltage (`VDDCPU_A`/`_B`) on the Odroid N2+, shown in a tmux
  status line next to htop. Rail mapping is measured, see `htopv-README.md`.
- `cpu_hotplug.py`, `cpu-hotplug.service`, `deploy_hotplug.sh`,
  `lowfreq_set_floor.sh`, `cpu-freq-guard.sh`/`.service`, `test_cpu_hotplug.py`
  + `LOWFREQ-README.md` — idle CPU hotplug and the persistent clock floor.
- `uv.py`, `uv_lowfreq.py`, `gen_lowfreq_dts.py`, `uv-opp-table.json` +
  `UV-README.md` — software undervolt (VDDCPU_A/B, DT floor 680 mV).
- `odroid-bootstrap.sh`, `odroid-desktop-diagnose.sh`, `odroid-esp32-inspect.sh`,
  `reverse_ssh.sh`, `test_agent.py` — Odroid/channel helpers that used to live in
  the firmware tree under `tools/remote-shell/` (moved here; `publish.py` now
  cleans the old paths).
- `novnc.service`, `xfce-vnc.service`, `vnc-xstartup`, `deploy_novnc.sh` +
  `novnc-README.md` — the XFCE desktop in the browser (TigerVNC + noVNC).

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

The deploy registers the Lovelace resource as
`/local/wp-zoom-chart.js?v=<content-hash>`. This is **required**: HA serves
`/local/*` with `Cache-Control: max-age=2678400` (31 days), so a resource URL
without a version query is cached for a month and every card update silently
never reaches the browser. The hash changes with the file, so each deploy
becomes a new URL and the browser reloads it.

## Tests

The pure logic is covered without mocks (no network, no HA needed):

```bash
python3 remote-shell/ha/test_ha_gateway_history.py
python3 remote-shell/ha/test_ha_wp_dashboard.py    # resource_url versioning
python3 remote-shell/ha/check_wp_zoom_chart.py   # needs node
```
