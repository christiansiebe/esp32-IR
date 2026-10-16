# idf-monitor-web — serieller IDF-Monitor im Browser

Host-Werkzeug für den **Odroid**. Macht `esp-idf-monitor` über eine
Web-Oberfläche bedienbar. Die **ESP32-Firmware bleibt unverändert** — der
serielle Port hängt am Odroid, nicht am ESP32.

```
Browser (xterm.js) <-> WebSocket <-> idf_monitor_web.py <-> PTY <-> esp-idf-monitor <-> /dev/ttyUSB0
```

## Warum es so gebaut ist

- **PTY ist Pflicht.** `esp-idf-monitor` braucht `isatty()` und Raw-Mode auf
  seinen eigenen stdio und setzt DTR/RTS per `ioctl` (Chip-Reset). Mit einer
  Pipe statt PTY scheitert es mit `[Errno 25] Inappropriate ioctl for device`.
  Mit dem PTY funktionieren **alle** Monitor-Tasten nativ (Ctrl+] beenden,
  Ctrl+T Menü, Ctrl+R Reset, Ctrl+P Bootloader).
- **Eigener stdlib-Server statt `ttyd`/`websockets`.** `ttyd` hat kein
  Debian-arm64-Paket; `websockets` würde ein zweites venv erzwingen. Der
  WS-Codec (`wsproto.py`) ist RFC-6455-konform und deckt genau das ab, was ein
  Terminal braucht.
- **Ein Prozess pro Port, Fan-out an viele Tabs.** Zwei Leser auf demselben
  Port erzeugen Datenmüll. Genau **ein** Monitor-Prozess; jeder Browser-Tab
  hängt sich an (eigene Ausgabeschlange, langsame Tabs bremsen niemanden).
- **Port-Autodetect nur USB.** Bevorzugt `/dev/serial/by-id/*`, dann
  `ttyUSB*`/`ttyACM*`. `ttyAML*`/`ttyS*` (Odroid-Konsole) sind **gesperrt**.

## Installation (auf dem Odroid)

```bash
./install.sh          # venv /opt/idf-monitor + systemd-Unit + Token
```

Der Token landet unter `~/.esp-idf-monitor-web/token` (600) und **nie** im
öffentlichen Repo.

## Start von Hand

```bash
python3 idf_monitor_web.py --port auto --baud 115200 --http-port 7681 \
        --monitor /opt/idf-monitor/bin/esp-idf-monitor
# Browser: http://<odroid>:7681/?token=<TOKEN>
```

Optionen: `--port` (Pfad oder `auto`), `--baud`, `--http-port`, `--bind`,
`--token`, `--monitor`, `--cfg`.

## Tastenkonflikte (Browser frisst Ctrl+T/R/P/F/L/I/Y/H)

Der Server schreibt automatisch eine Monitor-CFG (`state/esp-idf-monitor.cfg`)
und übergibt sie per `ESP_IDF_MONITOR_CFGFILE`. Umlagen lassen sich über
`build_monitor_cfg()` anpassen; nur Tasten, die als C0-Codes darstellbar sind,
sind zulässig (`A-Z`, `[ ] \ ^ _`). `exit_key` bleibt `]`.

## Sicherheit

- **Auth davor.** Der Monitor hat keine eigene; der Token aus dem Query ist der
  minimale Schutz. Für Netzzugriff einen Reverse-Proxy mit Basic-Auth/TLS
  davor setzen (`--bind 127.0.0.1` + Proxy).
- Kein TLS im Server; keine Secrets im Repo.

## Tests (Host, ohne Hardware)

```bash
python3 test/test_idf_monitor_web.py          # WS-Codec, Port-Filter, Auth, CFG
python3 remote-shell/idf-monitor-web/test_smoke.py   # End-to-End ohne Hardware
```

`test_idf_monitor_web.py` prüft den WS-Codec (RFC 6455), die Port-Ablehnung der
Board-Konsole, Auth und die Monitor-CFG. `test_smoke.py` startet den Server mit
einem eingebauten Fake-Monitor und fährt über echtes WebSocket: Index-Seite,
Token-Ablehnung, Banner, Tastaturecho, Resize. Beide brauchen keine Hardware und
kein Netz.
