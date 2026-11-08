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
- ESP32-C6 (single-core RISC-V, 4 MB flash) for the **receiver** role. Its top
  clock is 160 MHz (vs 240 on the classic) and its console is the on-chip **USB
  Serial/JTAG** (`/dev/ttyACM*`), not a UART bridge. The same source tree builds
  it; the target-specific knobs live in `sdkconfig.defaults.esp32c6`.
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
idf.py -p /dev/ttyUSB0 flash monitor      # ESP32 classic
idf.py -p /dev/ttyACM0 flash monitor      # ESP32-C6 (USB Serial/JTAG)
```

On the ESP32-C6 the bootloader offset is **0x0** (not 0x1000) and esptool talks
over the USB Serial/JTAG port, so the port is `/dev/ttyACM*` and no `--before`
reset trick is needed.

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

At runtime the console `set` command persists its values in NVS, so they survive
a reboot and an OTA (no reflash). For the **receiver** two radio-side knobs are
available in addition to the CPU/sleep ones: `set txpower <0|2..20>` (Wi-Fi TX
power in dBm) and `set beacon <0|100..6000>` (SoftAP beacon interval in TU,
1 TU = 1024 us). `0` keeps the chip/IDF default, so the classic-ESP32 receiver
build is unchanged until you opt in. Bench note (ESP32-C6): the idle draw is set
by the always-on Wi-Fi radio, not the CPU — the defaults measure ~0.433 W and
stacking every knob only reaches ~0.420 W (~3 %); see `AGENTS.md`.

### Status LED (ESP32-C6 receiver)

The ESP32-C6 receiver shows its state on the board's single addressable WS2812
RGB LED (default **GPIO 8**). It is a dim, slow "heartbeat": a short pulse every
1.5–2.5 s whose colour names the state, and the strip is written **only when the
colour or the on/off phase changes** (no continuous refresh, no animation loop).
The whole feature is compiled out on the sender and on the classic-ESP32 receiver
(where GPIO 8 is a SPI-flash pin); the LED only exists on the C6.

| State | Colour | Meaning |
| --- | --- | --- |
| `boot` | white | first ~1 s after start |
| `wifi` | amber | station associating to the home WLAN |
| `ap` | blue | no WLAN configured (SoftAP/setup mode) |
| `wait` | cyan | associated, no ESP-NOW data yet |
| `ok` | green | ESP-NOW data flowing (normal) |
| `error` | red | sender silent > 30 s, WLAN dropped, or web guard tripped |
| `ota` | magenta | OTA download in progress |

Three persisted knobs (NVS, like the other `set` keys) plus one RAM-only
diagnostic:

| Key | Range | Default | Effect |
| --- | --- | --- | --- |
| `set led <on\|off>` | on/off | `on` | master switch; `off` releases the RMT driver (0 idle current) |
| `set led-gpio <n>` | 0..48 | `8` | LED data GPIO; `0` disables the LED (clean no-op without LED hardware) |
| `set led-brightness <n>` | 0..255 | `16` | global brightness; `0` is dark |
| `set led-status <name\|auto>` | boot/wifi/ap/wait/ok/error/ota/auto | `auto` | force one state for bench work (RAM only, not persisted) |

`set led off` (or `led-gpio 0`) stops the tick and deletes the driver, so the
LED adds nothing to the receiver's idle draw. At the default brightness 16 the
lit current is a few mA at most and the ~5 % duty cycle keeps the *average* far
below 1 mA — negligible next to the always-on Wi-Fi radio (~0.43 W). The strip is
never refreshed while the colour and phase are unchanged, so the CPU cost is a
handful of writes per second at most.

## Repository notes

`AGENTS.md` holds the working notes: build gotchas, the flash layout, the
ESP-NOW wire format, the backfill protocol and the open tasks (`TODO.md`).
Read it before changing anything.

## License

The project's own source is **MIT** — see [`LICENSE`](LICENSE).

Bundled third-party code keeps its own license:

- [`components/sml_parser`](components/sml_parser) vendors
  [`olliiiver/sml_parser`](https://github.com/olliiiver/sml_parser) 0.29 under
  **LGPL-2.1**; the upstream `LICENSE` is kept in that directory. The SML
  decoding therefore links an LGPL-2.1 library, not MIT code.
- The managed components pulled by [`main/idf_component.yml`](main/idf_component.yml)
  and ESP-IDF keep their upstream licenses: `espressif/mqtt` (Apache-2.0),
  `espressif/cjson` (MIT), `espressif/mdns` (Apache-2.0) and ESP-IDF (Apache-2.0).
