# esp32-IR

Firmware for reading an IR / optical SML electricity meter (eHZ) with an
ESP32 and showing the values on a Wi-Fi dashboard. One source tree builds two
roles.

## What it does

- **Sender**: reads the meter's SML telegrams on UART1 (9600 baud 8N1, RX on
  GPIO16), decodes the OBIS values, and forwards them over **ESP-NOW**
  (broadcast, no Wi-Fi/IP of its own). It stores its own minute history in
  SPIFFS and uses an adaptive light sleep between telegram bursts to save power.
- **Receiver**: receives the ESP-NOW values, keeps the SPIFFS minute history
  (up to ~20 days) and serves the **Wi-Fi dashboard** (SoftAP `ESP32-IR-Meter`
  plus an optional station link to the home WLAN). It advertises
  `esp32-meter.local` via mDNS and can bridge the values to MQTT / Home
  Assistant.

No data is lost across radio gaps, restarts or OTA: the receiver pulls the
minutes it missed from the sender over ESP-NOW and merges them into its own
store (see `AGENTS.md`, section "History backfill").

## Hardware

- ESP32 (classic), 4 MB flash, **no PSRAM** required.
- Meter IR head on UART1 RX (GPIO16), 9600 baud 8N1.

## Build

The build runs in the official ESP-IDF container, so the host only needs
Docker (ESP-IDF v6.1):

```bash
tools/build.sh receiver        # or: tools/build.sh sender
tools/build.sh receiver --clean
```

Artifacts land in `build/receiver/` or `build/sender/` (`esp32-IR.bin` and the
flash args). `tools/build.sh` picks `docker` or `sudo docker` automatically;
start the daemon first if needed (`sudo dockerd &`).

## Flash

Use the standard IDF flow (the flash bundle includes bootloader, partition
table and app):

```bash
idf.py -p /dev/ttyUSB0 flash monitor      # inside an IDF shell / container
```

**Never erase the `storage` partition.** It holds the measured minute history
(`/powerlog/quarters.bin`, up to 29000 minutes). A full chip erase or a
`storage` flash destroys it. The partition sits at a fixed address
(`0x320000`, size `0xE0000`) and must stay there.

## Update (OTA)

Two ways, both keep the `storage` partition untouched:

1. **From the sandbox / any public host (pull OTA).** Publish the built image
   with `tools/ota-publish.sh <bin> [device-ip]`, then trigger the pull on the
   device (or from a LAN host):

   ```bash
   curl -sS -X POST 'http://<device-ip>/api/ota-pull' \
        -H 'X-OTA-Key: <wifi-key>' \
        -H 'Content-Type: application/json' \
        -d '{"url":"https://<public-host>/files/esp32-IR.bin"}'
   ```

   The device downloads over **HTTPS only**, validates the image, writes it to
   the update slot and reboots. The `/update` page also has a URL field for
   this.

2. **Upload a local file.** The `/update` page (receiver) accepts a `.bin`
   upload; `/update-sender` relays the image to the sender over ESP-NOW.

A freshly flashed image confirms itself after 45 s if the dashboard is up, and
rolls back to the previous slot if it is not (see `AGENTS.md`).

## Tests

Pure logic is host-tested (no hardware needed):

```bash
./test/run_host_tests.sh
```

This covers the SML parser, the power-history store (including reboot
persistence and record migration), the history backfill/dedupe, the adaptive
sleep controller, the link fallback, the boot/rollback policy and the web
guard.

## Configuration

Kconfig (`main/Kconfig.projbuild`) selects the role
(`CONFIG_ESPNOW_ROLE_SENDER` / `_RECEIVER`) and the ESP-NOW channel. Defaults
live in `sdkconfig.defaults` plus the per-role overrides in `tools/build.sh`.

## Repository notes

`AGENTS.md` holds the working notes: build gotchas, the flash layout, the
ESP-NOW wire format, the backfill protocol and the open tasks (`TODO.md`).
Read it before changing anything.

## License

MIT. See `LICENSE`.
