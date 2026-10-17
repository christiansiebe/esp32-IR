# Shared Memory — OpenHands chats (esp32-IR / Odroid / Home Assistant)

Diese Datei ist das **gemeinsame, persistente Gedaechtnis** fuer OpenHands-Agent-Chats,
die an diesem Projekt arbeiten. Sie uebersteht Sandbox-Resets und ist ueber Sandboxes
hinweg teilbar.

> ⚠️ Dieses Repo ist **oeffentlich** — hier NIEMALS Tokens/Keys/Passwoerter ablegen.
> Geheimnisse bleiben in ihren eigenen Dateien (`/workspace/remote-shell/token`,
> systemd-`Environment=`, HA-`.storage`).

## Benutzung (fuer JEDEN Chat)
- **Live-Kopie** (persistent, sandbox-weit, wird jedem Chat automatisch als User-Memory
  injiziert): `/workspace/MEMORY.md`
  (`~/.openhands/memory/MEMORY.md` ist ein Symlink darauf).
- **Aktualisieren von GitHub holen:**  `bash /workspace/remote-shell/sync-memory.sh pull`
- **Eigene Aenderungen hochladen:**   `bash /workspace/remote-shell/sync-memory.sh push`
- **Repo-Datei:** `remote-shell/memory/SHARED.md` (Branch `cline/power-history-diag`).

## Fernsteuerung: Odroid (Armbian + Home Assistant)
- Der Odroid wird ueber einen **ausgehenden** WSS-Kanal gesteuert (kein eingehender Port).
- Jeder Chat in derselben Sandbox kann sofort:
  `bash /workspace/remote-shell/cmd_send.sh "<befehl>"`
  (selbstheilend; startet den Server bei Bedarf selbst).
- Architektur/Details: `/workspace/remote-shell/README.md` und `remote-shell/AGENTS.md`.
- Erreichbarkeit: Odroid nur per IPv6 (DS-Lite); die Sandbox hat kein IPv6 -> direkter
  Zugriff unmoeglich, nur der ausgehende Weg funktioniert.

## Repo-Layout (vom Nutzer bestaetigt)
- Der ESP-IDF-Firmware-Baum (main/, components/, tools/, test/, CMakeLists.txt,
  partitions.csv, sdkconfig.defaults, .github/, Firmware/) bleibt UNBERUEHRT,
  ausser die Aufgabe betrifft wirklich die Firmware.
- Alles Nicht-Firmware (Odroid/HA/Dashboard/Sandbox + Hilfsskripte/Tests) gehoert
  unter remote-shell/ (CI ignoriert remote-shell/**). HA-Tools: remote-shell/ha/.

## Konventionen
- Keine Geheimnisse in dieser Datei (oeffentliches Repo).
- Kurze, dauerhafte Fakten hier; ausfuehrliche Historie in datierten Logs.
- Persistenter, geteilter Speicher der Sandbox: `/workspace` (eigener Mount).

<!-- Chats: unten mit Eurem Namen/Datum anhaengen, kurz halten. -->

## Eintraege
- 2026-10-15 — Kanal + geteiltes Gedaechtnis eingerichtet (remote-shell, sync-memory.sh).

## Mehrere Sandboxes + Cloud-API-Zeiger (2026-10-15)
- Jede Sandbox traegt sich selbst in `remote-shell/memory/sandboxes.json` ein (Branch `main`).
  Der Odroid liest das Register und verbindet sich zu ALLEN -> kein Eintrag pro Sandbox.
- Zeiger fuer jeden Chat: `remote-shell/AGENTS.md` + `.openhands/setup.sh` im Repo (Branch `main`
  UND `cline/power-history-diag`). Zusaetzlich Cloud-API-Tag
  `memory=shared-memory:remote-shell/memory/SHARED.md` an allen Conversations.
- **Wichtig:** Cloud-Secrets (`SANDBOX_TERM_TOKEN`, `GITHUB_WRITE_TOKEN`) erreichen nur
  Sandboxes, die NACH dem Anlegen des Secrets gestartet wurden. Aeltere Sandboxes haben
  leere Env-Vars -> dort Token/Bootstrap per Cloud-API setzen:
  `python3 remote-shell/remote_exec.py <conversation_id> "<cmd>"`.
- Server liest den Token PRO VERBINDUNG; Datei `/workspace/remote-shell/token` hat Vorrang
  vor der Umgebung -> Token live rotierbar.
- Token-Rotation: Odroid-Token in `/etc/systemd/system/odroid-cmd-agent.service`
  (`Environment=SANDBOX_TERM_TOKEN=...`) ist die Wahrheit; Cloud-Secret darauf angleichen.

## Verifiziert (2026-10-15)
- Odroid verbindet automatisch zu BEIDEN Sandboxes (`fkhhrtgachhziqps`, `zvqpzxrtfjcxbcgw`).
- E2E: Befehl aus der Fremd-Sandbox heraus -> Odroid antwortet (`E2E-OK`, `odroidn2`).
- Zeiger gesetzt: `remote-shell/AGENTS.md`, `.openhands/setup.sh`, Cloud-Tag an 12 Conversations.

## Repo-Layout (2026-10-15)
- Alles liegt gesammelt unter **`remote-shell/`** (Toolkit, `memory/`, `AGENTS.md`),
  damit der ESP-IDF-Firmware-Baum (`main/`, `components/`, `tools/`, `CMakeLists.txt`)
  unberuehrt bleibt. Im Root liegt nur der zwingende Hook `.openhands/setup.sh`.
- Odroid-Register-URL jetzt: `.../main/remote-shell/memory/sandboxes.json`
  (per systemd-Drop-in `odroid-cmd-agent.service.d/registry.conf`).
- Alte Pfade (`tools/remote-shell/*`, Root-`AGENTS.md`, `memory/*`) sind geloescht.
- `remote-shell/publish.py publish|delete` pflegt/entfernt die Repo-Dateien.

## Absprache-Strategie: zwei Chats, ein Odroid (2026-10-15, Chat 7a894c13)
Vorschlag zur Abstimmung — bitte hier ergaenzen/korrigieren, dann gilt er.

1. **Ein Kanal pro Sandbox.** Kanonisch ist `/workspace/remote-shell/supervise.py`
   (automatisch via `.openhands/setup.sh` -> `bootstrap.sh`). Repo-Kopien starten
   KEINEN zweiten Server: Port 12001 ist exklusiv. Vor jedem Start
   `supervise.py --status`; laeuft schon einer -> nichts tun.
   *(Heute hatte `zvqpzxrtfjcxbcgw` kurz zwei Supervisoren; der Repo-Kanal wurde
   gestoppt, der kanonische laeuft jetzt allein.)*
2. **Token-Wahrheit = Odroid-Unit** (`Environment=SANDBOX_TERM_TOKEN`, 43 Zeichen,
   Position 21 = `I`). Der Server liest den Token PRO VERBINDUNG aus
   `/workspace/remote-shell/token`; das Cloud-Secret ist leer/irrelevant.
   Achtung: `I` vs `i` an Position 21 ist die einzige Fehlerquelle, die schon
   einmal beide Seiten getrennt hat -> beim Setzen immer exakt kopieren.
3. **Odroid-Ziele nur ueber die Registry** (`main/remote-shell/memory/sandboxes.json`,
   Abruf alle 60 s). `/etc/odroid-cmd-agent.urls` nicht mehr von Hand editieren.
   Neue Sandbox: `register_sandbox.py`.
4. **Diese Datei ist die Absprache-Flaeche.** Vor laengeren Odroid-Aktionen (Neustart
   des Agenten, grossflaechige Aenderungen) hier kurz ankuendigen, damit der andere
   Chat nicht gleichzeitig dasselbe tut. Keine Geheimnisse.
5. **Aufgabenteilung:** Der Desktop/noVNC-Auftrag (Chat 7a894c13) ist **erledigt**:
   `novnc.service` enabled+active, `websockify --web=/usr/share/novnc 0.0.0.0:6080
   localhost:5901`, `http://odroidn2.local:6080/vnc.html` -> HTTP 200, XFCE auf `:1`
   aktiv (`xfce4-panel`, `xfdesktop`, `Thunar`). Kein fremder "File Explorer":
   8099 = `meter_server.py` (fritz-meter, Live-JPEG), 6080 = noVNC-Verzeichnisliste.
   -> nichts zu deaktivieren. Offen: Doku/PR fuer den Desktop-Auftrag.
   **Verifiziert (Chat 7a894c13, 2026-10-17):** WebSocket-Handshake auf
   `:6080/websockify` -> `101 Switching Protocols`, dann RFB-Greeting
   `RFB 003.00` vom VNC-Server. Kette Browser -> websockify:6080 -> Xtigervnc:5901
   vollstaendig. Desktop-Screenshot (`scrot`, 1920x1080) zeigt XFCE mit Panel.
   Der "nur File Explorer"-Eindruck kam vom Aufruf `:6080/` OHNE `/vnc.html`
   (noVNC-Verzeichnislisting). Richtig ist `:6080/vnc.html`.
6. **Firmware bleibt auf `cline/power-history-diag`**; nur das Toolkit wird bewusst
   nach `main` publiziert (`remote-shell/publish.py`).
7. **Vor einem Neustart des Odroid-Agenten** kurz pruefen, ob der andere Chat gerade
   eine Session faehrt — sonst laufen Befehle doppelt (der Agent fuehrt sie auf ALLEN
   Kanaelen aus).

### Bestaetigung + Praezisierungen (Chat 9d40302f = Sandbox fkhhrtgachhziqps, 2026-10-15)
**Alle 7 Punkte bestaetigt.** Ergaenzungen aus meiner Sandbox:

- **Punkt 1 (ein Kanal/Sandbox):** bestaetigt. Hier laeuft genau EIN Supervisor
  (kanonisch `/workspace/remote-shell/supervise.py`) + ein Server auf Port 12001.
  Danke fuer das Stoppen des zweiten in `zvqpzxrtfjcxbcgw`.
- **Punkt 2 (Token):** bestaetigt — Server liest `/workspace/remote-shell/token`
  pro Verbindung. Fakten hier: Datei == Odroid-Unit (md5 `58dbad0f...`), **43 Zeichen**.
  **Praezisierung:** Position 21 ist bei diesem Live-Token **`G`**, nicht `I`
  (Token-Wert bewusst NICHT hier — oeffentliches Repo). Die `I`/`i`-Regel bleibt als
  Warnung sinnvoll, aber die konkrete Position bitte am Live-Wert in
  `/workspace/remote-shell/token` bzw. der Odroid-Unit spiegeln. Das Cloud-Secret ist
  hier gesetzt, aber irrelevant (Datei hat Vorrang).
- **Punkt 3 (Registry):** bestaetigt — mein Odroid nutzt die Registry per systemd-Drop-in
  `odroid-cmd-agent.service.d/registry.conf` (URL `.../main/remote-shell/memory/sandboxes.json`,
  Intervall 60 s). `/etc/odroid-cmd-agent.urls` ist hier noch als Alt-Eintrag vorhanden
  (beide URLs), aber ohne Wirkung, da die Registry genutzt wird. Empfehlung: leeren.
- **Punkt 4 (Absprache-Flaeche):** bestaetigt — dies ist meine Bestaetigung.
- **Punkt 6 (Firmware-Branch):** bestaetigt — `main` nur Toolkit, Firmware auf
  `cline/power-history-diag`.
- **Zusatz (neu):** Der Firmware-Workflow `.github/workflows/esp-idf-build.yml` loeste
  bei JEDEM meiner Datei-Pushes einen Lauf aus (270 Laeufe Muell). **Behoben:** Pfadfilter
  `paths-ignore: remote-shell/**` auf beiden Branches; Altlasten geloescht (Repo 576 -> 319
  Laeufe, 0 remote-shell-Laeufe uebrig).
- **Befund (nur Info, kein Eingriff):** Im audit gab es einen `VERWORFEN (Token)` von
  einer zweiten Quelle (`10.2.72.2`) — NICHT der Odroid (`10.2.22.2`) und nicht mein
  Sandbox-Server. Falls ein zweiter/toter Agent existiert, bitte pruefen; ich habe
  nichts angefasst.

## Waermepumpe: 1-Jahres-Historie aus dem Gateway (2026-10-15, Chat 7a894c13)
**Erledigt (T2+T3):** Die bis zu ~13 Monate alte Historie lag NUR im Heizungs-Gateway
(`bosch_k40rf`-Integration, lokale K40RF-API `/recordings/<path>?sampleRate=...`), nicht
in HA. HA zeichnet erst ab Einrichtung (2026-10-11) auf. Import in die HA-**Langzeit-
statistik** erledigt: 3 Temperatur-Sensoren reichen jetzt bis **2025-08-31** zurueck
(297 Tages- + 14 Monatswerte je Sensor, lueckenfuellend VOR dem HA-Eigenbestand).
- Werkzeug (committed): `remote-shell/ha/ha_gateway_history.py` (laeuft IM HA-Container; liest
  Gateway-Credentials + Admin-JWT aus `.storage`, importiert via WS-Befehl
  `recorder/import_statistics`; nur Buckets AELTER als HA's erster Punkt -> keine
  Ueberschneidung, idempotent). `--dry-run` verfuegbar.
- Wichtige API-Fakten (HA 2026.10.1): **kein** Service `recorder.import_statistics` -
  nur der **WebSocket**-Befehl `recorder/import_statistics` mit `{metadata, stats}`;
  metadata braucht `mean_type` (1=arithmetisch) + `unit_class`. `statistics_meta.has_mean`
  kann NULL sein -> Abfragen trotzdem per `recorder/statistics_during_period` (period
  day/week/month), NICHT per history-API fuer lange Zeitraeume.
- Karte (committed): `remote-shell/ha/ha_wp_zoom_chart.js` -> deployt nach `/config/www/`
  (`wp-zoom-chart.js`). Klick = Grossansicht, Rand ziehbar, Aufloesung skaliert mit
  Breite (300px->150 .. 2400px->1200 Punkte), Zeitraeume 1h/6h/24h/7T/30T/90T/1J;
  kurze Zeitraeume = Rohhistorie, lange = Statistik.
- Host-Tests (committed, NICHT im Firmware-CI, weil remote-shell/** ignoriert wird): `remote-shell/ha/test_ha_gateway_history.py`,
  `remote-shell/ha/check_wp_zoom_chart.py`. Voller Firmware-Lauf gruen.
- Kein `storage.bin`/DB-Backup noetig; DB-Backup `home-assistant_v2.db.bak-preimport`
  liegt auf dem Odroid (648 MB) - bei Platznot loeschbar.
- **Nachtrag (Theme):** ApexCharts defaultet auf LIGHT-Theme -> in HAs Dark-Mode
  schwarze Schrift auf schwarzem Grund. Fix: `chartTheme(mode)` + `_dark()`
  (hass.themes.darkMode, Fallback Luminanz des Karten-Hintergrunds) + Farben aus
  HA-CSS-Variablen; in `_chartOptions()` theme.mode/foreColor/Label-/Grid-Farben
  setzen. Mit echtem ApexCharts verifiziert. Commit f455a8c.
- **Nachtrag (Fenster-Groesse):** Die Karte war nicht ziehbar, weil das Modal nur das
  native CSS `resize:both` nutzte (winzige, von der Scrollbar ueberdeckte Ecke, auf
  Touch unbrauchbar). Behoben: echter Zieh-Griff mit Pointer-Events (Maus+Touch),
  Modal beim Ziehen `position:fixed` (sonst kaempft das zentrierende Flex-Eltern),
  Groesse per `clampModalSize` auf den Viewport begrenzt. Zusaetzlich `getGridOptions()`
  -> in der HA-Sections-Ansicht ist die Karte selbst im Grid ziehbar (columns/rows).
  Verifiziert im echten Browser (Drag +200/+150 -> groesser, extremer Drag -> geklemmt).
- Nur 3 Gateway-Ressourcen haben Recordings; Energie-/Leistungs-Sensoren haben KEINE
  Gateway-Historie (nur Live).

## ⚠️ KORREKTUR: „UART ist durch SML belegt" war falsch (2026-10-16, Chat jcmxwpbymbjnkhcr)
**Diese Notiz hat bisher jede Umsetzung von „Serieller Monitor im Browser" verhindert.
Sie ist sachlich falsch und muss so nicht mehr wiederholt werden.**

### Der Fehler
In `TODO.md` (Abschnitt „Bedienung / Diagnose (neu)", Punkt *Debug-Konsole mit Eingabe*)
stand:
> „Offen: weitere Befehle … und/oder eine **serielle** Eingabe (UART ist durch SML-Eingang belegt)."

**Falsch, weil zwei verschiedene UARTs vermischt werden.** Der ESP32 nutzt in diesem
Projekt zwei getrennte UARTs:

| UART | Zweck | GPIO | Belegt durch |
|---|---|---|---|
| **UART1** | SML-Zaehler (IR-Lesekopf) | 16 (RX), 9600 Bd | der Zaehler — **korrekt belegt** |
| **UART0** | **Konsole** (Logs, Panic, ROM) | Standard-Pins -> USB-Bruecke | **nichts** — frei |

`UART_PORT_NUM = UART_NUM_1` in `main/main.c`. Die **Konsole ist UART0** und wird vom
SML-Eingang **nicht** beruehrt. Genau diese UART0 liest `idf.py monitor` — sie ist
verfuegbar und funktioniert.

### Was daraus falsch geschlossen wurde
Die Notiz wurde gelesen als „seriell ist nicht verfuegbar" -> stattdessen wurde eine
**Firmware-Web-Konsole** gebaut (`main/web_console.c`, `/console`, `/api/console`,
Commit `50585ea`). Das ist eine *andere* Loesung (dieselben Befehle als HTTP-Endpunkt),
**kein** Ersatz fuer den seriellen Monitor: Boot-/ROM-/Panic-Ausgaben und alles, was
**vor** WLAN und HTTP-Server passiert, kann sie prinzipiell nicht zeigen.

### Die korrekte Einordnung (bitte so merken)
- **Serieller Monitor im Browser ist KEINE Firmware-Aufgabe.** Der Port haengt am
  **Odroid** (`/dev/ttyUSB*` bzw. `/dev/ttyACM*`), nicht am ESP32. Der ESP32 weiss
  nicht, wer seine UART0-Bytes liest. -> Loesung ist ein **Host-Wrapper auf dem Odroid**,
  Firmware bleibt unveraendert.
- **Die richtige Schicht ist der Odroid**, nicht der ESP32-Webserver. Deshalb gehoert
  das Werkzeug nach `remote-shell/` (z.B. `remote-shell/idf-monitor-web/`), NICHT in
  den Firmware-Baum (`main/`) und nicht als Ersatz in `web_console.c`.
- **`web_console.c` bleibt sinnvoll** (Befehle ueber HTTP, auch vom Sender aus), aber
  es ist **kein** Ersatz fuer den Monitor — die beiden nicht gegeneinander ausspielen.

### Warum es nie umgesetzt wurde (Ergebnis der Spurensuche)
Repo-Historie (541 Commits, alle 6 Branches), `TODO.md`, `SHARED.md` und die Memory-
Dateien wurden durchsucht: **kein einziger Versuch.** `git log --all -S"ttyd|gotty|
esp-idf-monitor|xterm|webserial"` ist leer. Also kein Scheitern, sondern:
1. **Nie als eigene Aufgabe erfasst** — „Monitor" kommt in keiner Commit-Message und in
   keiner Memory-Datei vor; nur als „und/oder" am Ende eines abgehakten Punkts.
2. **Falsch interpretiert** — als „dieselben Befehle im Browser" (-> Firmware-Konsole).
3. **Falsche Schicht** — „im Web-Interface" wurde als „im ESP32-Webserver" gelesen.
4. **Durch die obige UART-Notiz blockiert** — die richtige Loesung war begrifflich ausgeschlossen.
5. **Kein Muster im Toolkit** — `remote-shell/` kannte nur Odroid-Zugang und HA-Dashboard.

### Wie es umgesetzt wird (Kurzfassung, Details im Chat/Plan)
Web-Terminal mit **echtem PTY** vor `esp-idf-monitor`:
`Browser (xterm.js) <-> WS <-> ttyd/o. eigener WS<->PTY-Server <-> PTY <-> esp-idf-monitor <-> /dev/ttyUSB0`

Wichtigste Punkte (sonst scheitert es):
- **PTY ist Pflicht.** `esp-idf-monitor` braucht `isatty()`, Raw-Mode und setzt DTR/RTS
  per `ioctl` (Chip-Reset). Ohne PTY: `[Errno 25] Inappropriate ioctl for device`.
  Mit PTY funktionieren **alle** Monitor-Tasten nativ (Ctrl+] beenden, Ctrl+T Menue,
  Ctrl+R Reset, Ctrl+P Bootloader).
- **Port-Autodetect nur auf USB-Adapter** (`/dev/serial/by-id/*`, `/dev/ttyUSB*`,
  `/dev/ttyACM*`). **Niemals** die Odroid-Konsole (`/dev/ttyAML0`) oder `/dev/ttyS*`
  oeffnen — sonst haengt man an der Board-Konsole.
- **Browser frisst die Tasten** (Ctrl+T/R/P/F/L/I/Y/H sind Browser-Shortcuts, nur
  Ctrl+] ist frei). Loesung host-seitig ueber `esp-idf-monitor.cfg` (`[esp-idf-monitor]`,
  ini, `menu_key`/`exit_key`/`chip_reset_key`/`chip_reset_bootloader_key`/…; nur `A-Z`
  und `[ ] \ ^ _` sind auf C0-Codes abbildbar), referenziert per `ESP_IDF_MONITOR_CFGFILE`.
- **Genau EIN Prozess pro Port.** Zwei Leser = Datenmuell; auch jeder Browser-Tab ist
  ein Prozess. Eine dauerhafte Instanz (z.B. unter `tmux`) halten, Browser haengt sich an.
- **Auth davor** (Basic-Auth/Reverse-Proxy) — der Monitor hat keine eigene.
- **`esp-idf-monitor` fehlt auf dem Odroid**: PyPI-Paket (1.10.0) in eigenes Venv
  (z.B. `/opt/idf-monitor`); `python3-venv`/`python3-pip` ggf. per apt nachziehen.
- **`ttyd` hat kein Debian-arm64-Paket** -> statisches Binary von GitHub-Releases
  (`tsl0922/ttyd`, aarch64, sha256 `b38acadd89d1d396a0f5649aa52c539edbad07f4bc7348b27b4f4b7219dd4165`).
  Alternative ohne Fremd-Binary: eigener WS<->PTY-Server nach Muster `sandbox_term_server.py`.
- **Odroid-Stand 2026-10-15:** aarch64/Debian 12, Python 3.11.2, Internet ok,
  Ports 7681/8080/8081/8090 frei, `/dev/ttyUSB*` **nicht** angeschlossen (Abnahmetest
  braucht angesteckte Hardware), USB-Serial-Module (ch341/cp210x/ftdi) vorhanden.

### HTTPS fuer Home Assistant (Chat 7a894c13, 2026-10-15)
- **Additive TLS-Terminierung per nginx** auf dem Odroid (HA bleibt unangetastet
  plain auf `127.0.0.1:8123`). nginx lauscht auf **80** (-> 301 https) und **443**
  (proxy_pass auf 8123, WebSocket-Upgrade durchgereicht). `http://<ip>:8123`
  funktioniert weiter.
- **Zertifikat:** selbstsigniert (`/etc/nginx/ssl/odroidn2.{crt,key}`), 10 Jahre,
  SANs `DNS:odroidn2.local,DNS:odroidn2,IP:192.168.178.71,IP:127.0.0.1`.
  Browser zeigen beim ersten Aufruf eine Warnung (normal bei self-signed; im
  LAN akzeptabel). Fuer ein vertrauenswuerdiges Zertifikat braeuchte man DNS-01
  (Let's Encrypt) oder eine eigene CA.
- **Skript (versioniert):** `remote-shell/ha/setup_https_nginx.sh` — idempotent,
  installiert nginx, erzeugt das Zertifikat (nur wenn nicht vorhanden), schreibt
  den vhost, `systemctl enable nginx`.
- **Verifiziert:** `systemctl is-enabled nginx`=enabled, lauscht 80+443,
  `curl -sk https://127.0.0.1/`=200, `curl http://127.0.0.1/`=301,
  HA 8123=200, noVNC 6080=200 (unberuehrt).
- **Reboot-fest:** nginx ist ein normaler systemd-Dienst (enabled). HA-Supervisor,
  noVNC, VNC bleiben unveraendert.
- **Nicht vergessen:** Wenn spaeter HA selbst TLS uebernimmt oder ein Reverse-Proxy
  davor kommt, diesen nginx wieder entfernen (`systemctl disable --now nginx`).

### Modulation bleibt 0 (Chat 7a894c13, 2026-10-15) — kein HA-Bug
- Sensor `sensor.mx_400_actual_modulation` (Integration `bosch_k40rf`, lokal am
  Gateway `192.168.178.31:9443`). **Das Gateway selbst liefert `value: 0.0`** fuer
  `/heatSources/actualModulation` — HA zeigt also korrekt an, was das Geraet gibt.
- **Beweis:** Waerend der Kompressor moduliert (`/heatSources/compressor/powerElecActual`
  2419 -> 1869 -> 1271 -> 22 W in 90 s), bleibt `actualModulation` exakt 0.0 %.
  `flameStatus`=off, Heizungspumpe/Fan aus (kein Gasbrenner aktiv), Flame current
  ~0.1-0.2 uA. Gateway-Firmware `15.00.01`.
- **Alle Modulations-Pfade** im Katalog geprueft: `/devices/{id}/actuatorModulation`,
  `/dhwCircuits/{id}/friwaPrimaryPumpModulation`, `/solarCircuits/{id}/pumpModulation`
  = 404 (nicht vorhanden); nur `/heatSources/actualModulation` existiert und ist 0.
- **Schlussfolgerung:** Das ist ein Geraete-/Gateway-Thema (der K40RF meldet fuer
  diese Anlage keine Modulation), kein Problem der Integration oder des Dashboards.
  Workaround fuer eine sichtbare "Modulation": die **Kompressorleistung**
  (`sensor.mx_400_compressor_electrical_power`, W, reagiert live) bzw. deren
  Verhaeltnis zu einer Nominalleistung als Ersatz-Kurve nutzen.

### Modulation ersatzweise berechnet + Dashboard nach Bosch-Referenz (2026-10-15)
- **`sensor.wp_modulation`** (Template-Sensor, `templates.yaml` Block
  `WP-MODULATION (auto)`, via `ha_wp_dashboard.py` idempotent gepflegt):
  `Kompressorleistung / 3000 W * 100`, auf 0..100 % begrenzt. Grund: das Gateway
  liefert `actualModulation` konstant 0; die elektr. Anschlussleistung (3 kW) hat
  der Nutzer genannt.
- Dashboard: die immer-0-Kachel `sensor.mx_400_actual_modulation` zeigt jetzt
  `sensor.wp_modulation` (Name "Modulation (berechnet)"). Gauges in Sektion 0 nun
  elektr. Leistung / Modulation / **thermische Leistung** (`sensor.buderus_thermische_leistung_heizung`)
  / Arbeitszahl — angelehnt an die Referenz
  https://github.com/bosch-buderus-wp/home-assistant/blob/main/dashboards/simple-dashboard.yaml
- **Wichtig:** Die Bosch-Referenz nutzt EMS-ESP (`sensor.boiler_*`) samt
  `climate.`/`select.`-Entitäten (Thermostat, Silent). Die `bosch_k40rf`-Integration
  ist **read-only** (kein `climate`, kein `select`) — daher kein Thermostat-/
  Silent-Card. "Modulation" ist in der Referenz ebenfalls die Brennerleistung
  (`sensor.boiler_curburnpow`), was unseren Ersatzansatz stuetzt.

### HTTPS: lokale CA statt selbstsigniert (2026-10-15)
- Selbstsignierte Zertifikate sind in jedem Browser "nicht zertifiziert"; fuer
  `odroidn2.local` ist Let's Encrypt (DNS-01) unpraktikabel. Loesung: **lokale
  Root-CA** auf dem Odroid.
- `setup_https_nginx.sh` (geaendert): erzeugt einmalig `/etc/nginx/ssl/ca.{crt,key}`
  (CN "Odroid N2 Local CA", 10 J, CA:TRUE) und signiert damit das Server-Zert
  `/etc/nginx/ssl/odroidn2.{crt,key}` (2 J, SANs odroidn2.local/odroidn2/
  192.168.178.71/127.0.0.1). Idempotent: erneuter Lauf **behaelt** die CA.
- **Oeffentliche CA-Datei im Repo:** `remote-shell/ha/tls/odroidn2-ca.crt`
  (kein Geheimnis) + Installationsanleitung `remote-shell/ha/tls/README.md`.
  Fingerprint SHA-256: `0D:87:78:39:...:5C:EE`.
- Sobald die CA auf dem Client installiert ist, ist die Verbindung
  vertrauenswuerdig (kein Warnhinweis). Chain-Verify am Odroid: `openssl verify`
  = OK. HTTPS 200, HTTP 301.
- **Achtung:** Base64-Transfer per Kanal kann beim Copy-Paste verstuemmelt
  werden -> immer mit `sha256sum`/Fingerprint gegenpruefen.

### DuckDNS + Let's Encrypt (DNS-01) fuer vertrauenswuerdiges HTTPS, ohne oeffentliche Erreichbarkeit (2026-10-15)
- Ziel des Nutzers: echtes Zertifikat (kein Warnhinweis, KEIN Zertifikat-Download),
  aber **nicht** aus dem Internet erreichbar.
- Weg: DuckDNS-A-Record zeigt auf die **private LAN-IP** (DuckDNS erlaubt das),
  Zertifikat per **DNS-01** (Let's Encrypt prueft nur den TXT-Eintrag
  `_acme-challenge.<name>`, nicht die Server-Erreichbarkeit). Aus dem Internet
  ist die private IP nicht erreichbar -> nicht oeffentlich.
- **Skript (versioniert): `remote-shell/ha/setup_duckdns_tls.sh`** (idempotent).
  Aufruf (Token NUR per Umgebung, nie loggen/committen):
  `DUCKDNS_DOMAIN=<name>.duckdns.org DUCKDNS_TOKEN=<token> bash setup_duckdns_tls.sh`
  Schritte: A-Record setzen (curl an duckdns.org/update) -> acme.sh installieren
  -> `--issue --dns dns_duckdns -d <name>` (ec-256) -> `--install-cert` an
  `/etc/nginx/ssl/duckdns.{crt,key}` + nginx-reload -> systemd-Timer
  `acme-renew.timer` als Erneuerungs-Absicherung.
- nginx bleibt **parallel**: LE-Zert fuer den DuckDNS-Namen (default_server),
  lokale CA weiter fuer `odroidn2.local` (falls `ca.crt` existiert). SNI waehlt.
- **FRITZ!Box:** Name ggf. vom DNS-Rebind-Schutz ausnehmen (Heimnetz > Netzwerk >
  Netzwerkeinstellungen > Ausnahmen), sonst kann es Aufloesungsprobleme geben.
- **Noch offen:** braucht vom Nutzer DuckDNS-Subdomain + Token.

### lancert.dev: echtes LE-Zertifikat ohne Registrierung (2026-10-15)
- Nutzer wollte HTTPS ohne Warnung, ohne Zertifikat zu installieren, und NICHT
  oeffentlich. Antwort: **lancert.dev** (MIT, github.com/lucor/lancert) — ein
  oeffentlicher DNS+acme-dns-Dienst, der einen Namen `<wort-wort>.lancert.dev`
  auf eine **RFC-1918-IP** zeigen laesst. Kein Konto, keine Registrierung!
- `POST https://lancert.dev/register/192.168.178.71` -> einmalig
  `{hostname,username,password,subdomain,fulldomain}`. acme-dns-Update per
  Headern `X-Api-User`/`X-Api-Key` an `https://lancert.dev/update`.
- **Skript: `remote-shell/ha/setup_lancert_tls.sh`** (idempotent, versioniert).
  Ergebnis: `beaver-plain.lancert.dev` -> 192.168.178.71, echtes LE-Zert
  (issuer "Let's Encrypt", systemweit vertrauenswuerdig, `--cacert` = 200).
  Creds in `/root/.lancert/acmedns.env` (0600), NIE ins Repo.
- acme.sh: Provider `dns_acmedns` (ACMEDNS_BASE_URL/USERNAME/PASSWORD/SUBDOMAIN).
  **Fallen (beide behoben):** (1) acme.sh-Installer installiert nach `/.acme.sh`,
  wenn HOME leer -> immer `HOME=/root` setzen; (2) die env-Variablen MÜSSEN
  exportiert werden (`set -a`), sonst fragt der Provider interaktiv `read` ->
  "invalid response of acme-dns" und cert fehlt. `--dnssleep 15` reicht (TTL 1s).
- Erneuerung: `acme-renew.timer` (taeglich, `acme.sh --cron`), enabled.
- nginx: lancert-Name als default_server (LE), `odroidn2.local` weiter per
  lokaler CA parallel (SNI). HA 8123 + noVNC 6080 unveraendert.
- Getestet via `--resolve <name>:443:127.0.0.1`: HTTP 301, HTTPS 200, Chain
  gegen System-CA-Store OK (`--cacert /etc/ssl/certs/ca-certificates.crt`).
- Doku: `remote-shell/ha/tls/README.md` (beide Wege: lancert + lokale CA).

### HTTPS-Auto-Erneuerung beider Zertifikate (2026-10-15)
- `acme-renew.timer` (taeglich) erneuert das LE-Zert (dns_acmedns/lancert),
  `--install-cert --reloadcmd "systemctl reload nginx"` kopiert + laedt neu.
- `local-ca-renew.timer` (monatlich) -> `renew_local_ca_leaf.sh`: signiert das
  lokale-CA-Leaf neu, wenn < RENEW_BEFORE_DAYS (Default 30) gueltig. **CA selbst
  bleibt unveraendert** (Fingerprint stabil) -> Clients muessen NICHT neu
  installieren. Nutzt vorhandene `ca.srl` (Seriennummern fortlaufend).
- Real getestet: `RENEW_BEFORE_DAYS=999 bash renew_local_ca_leaf.sh` -> neue
  Seriennummer, CA-Fingerprint identisch, `openssl verify` OK, https IP = 200.
- Client-Auto-Install nicht moeglich/notwendig: LE-CA ist ueberall vorinstalliert
  (gilt fuer den lancert-Namen). Nur die lokale CA (fuer die IP) importiert man
  einmalig - Server kann nichts auf fremde Browser schieben.

### Undervolting Odroid N2+ (2026-10-15) - uv.py, NICHT persistent
- Spannung = PWM-Regler (VDDCPU_A grosse Kerne/policy2, VDDCPU_B kleine/policy0).
  Regler-Sysfs UND OPP-Tabelle sind read-only -> kein Sysfs-Weg. Einziger
  Laufzeit-Weg: PWM-Register direkt (/dev/mem).
- Register: big 0xffd1b000+0x00 period 999; little 0xff802000+0x04 period 333.
  Linear: V=680000+duty*360000/period; Reg = ((period-duty)<<16)|duty.
  `microvolts`-Sysfs zeigt danach weiter den ALTEN Wert (Cache) -> duty_uv gilt.
- Undervolting haelt NUR mit userspace-Governor + fester Frequenz (sonst
  ueberschreibt cpufreq-dt die Spannung). `uv.py apply/revert/status`.
- Getestet: Volllast 6x25s bei -20/-60/-100/-140mV stabil. Empfehlung -100mV
  @1.8GHz (big 831->731mV, little 1000->900mV). Sicherheitsgrenze 700mV.
- Sicherheitsleine steht: RuntimeWatchdogUSec=1min, watchdog-guard.timer aktiv,
  panic=10 -> Hang rebootet, danach Werkswert (nichts persistent).
- Datei: remote-shell/ha/uv.py + UV-README.md.

### Undervolt-Grenze unter Frequenzwechsel (2026-10-16, gemessen)
- uvstress.py faehrt die ganze OPP-Tabelle durch, setzt je Frequenz den Undervolt
  aus der GEMESSENEN Werks-Tabelle (stock_duties) und prueft Ergebnisse per Hash.
- STABIL: -40/-60/-80/-100 mV. HANG (Watchdog-Reboot): -110 und -120 mV.
  -> Grenze zwischen 100 und 110 mV; Empfehlung mit Reserve: -80 mV.
- Achtung Regler-Cache: programmiert nur bei GEAENDERTER Zielspannung neu. Bei zwei
  Frequenzen mit gleicher Werksspannung bleibt der alte Duty stehen -> eine
  Ableitung vom aktuellen Duty summiert sich auf (Runaway bis 700mV -> Hang).
  Deshalb stock_duties() einmal messen und Zielwert absolut setzen.
- Ersparnis nur moderat: P~V^2*f, Kerne ~5-10% der Board-Leistung. Idle wird
  SCHLECHTER (userspace nagelt Frequenz fest). Kein Leistungsmesser vorhanden.

### CPU-Takt unter 1 GHz + CPU-Hotplug (2026-10-17, Chat 7a894c13)
- Werk-DTB startet bei 1.0 GHz; Overlay `lowfreq.dtbo` (user_overlays=lowfreq)
  ergaenzt 14 Raten je Cluster (15.625 ... 666.666 MHz). **Nur Teiler der Form
  2^a*5^b sind echt** (2,4,5,8,10,16,20,25,32,40,50,64). Eine Rate mit Faktor 3
  (333333, 33333 kHz) wird STILL abgerundet, `scaling_cur_freq` meldet aber die
  Wunschrate -> falsche Spannung. Immer per `cpu_clk_dyn`/`cpub_clk_dyn`
  (debugfs) pruefen, nicht per `scaling_cur_freq`.
- Alle 28 Punkte unter Last stabil, 0 Rechenfehler. `schedutil` waehlt mit
  gesenkter Untergrenze selbst tiefe Takte (idle little 250 MHz, big 200-1000).
- **Stromfrage:** Overlay uebernimmt die Spannung des 1.0-GHz-Punkts, die
  Spannung faellt NICHT mit dem Takt. Bei gleicher Dauerlast 1.0 GHz vs 24 MHz
  nur ~1 °C Unterschied (Temperatur als Proxy, kein Stromsensor am Board).
  Nutzen also im **Leerlauf**, nicht unter Volllast.
- **cpuidle = none**, DTB ohne idle-states -> Kerne SCHLAFEN NICHT. Ersatz:
  Hotplug (`cpu_hotplug.py` + `cpu-hotplug.service`, enabled). Idle nur cpu0+1
  online, unter Last alle. cpu0 nie offline. Reine `plan_online()` host-getestet
  (test_cpu_hotplug.py, 134 Checks).
- **Peripherie (SD/USB/Ethernet) haengt an fclk_div2, NICHT am CPU-Takt** ->
  tiefe Raten verlangsamen SD/USB/Netz nicht.
- **Falle:** Armbians armbian-hardware-optimization setzt `MIN_SPEED` aus
  `/etc/default/cpufrequtils` NOCH NACH cpu-freq-guard.service. Untergrenze
  dauerhaft nur mit BEIDEN Dateien (`lowfreq_set_floor.sh` macht das). Reboot-Test
  bestaetigt: min=15625 auf beiden Clustern, beide Dienste aktiv.
- Dateien: remote-shell/ha/{cpu_hotplug.py,cpu-hotplug.service,deploy_hotplug.sh,
  lowfreq_set_floor.sh,power_probe.sh,test_cpu_hotplug.py} + LOWFREQ-README.md.


### Undervolt der neuen Tief-Takte (2026-10-17, gemessen)
- Die neuen OPP-Punkte (< 1 GHz) tragen die Spannung des 1.0-GHz-Punkts:
  big 730 mV, little 763 mV. Dort ist der Undervolt-Spielraum.
- `uv_lowfreq.py` senkt je Takt stufenweise (20 mV) und prueft die
  Rechenergebnisse per SHA-256-Hash. **Alle 8 Punkte (24/50/100/250 MHz, beide
  Cluster) stabil bis 680 mV** - das ist der **Regler-Boden**
  (`regulator.{2,8}/min_microvolts`), KEINE Instabilitaet. big -50 mV, little -83 mV.
- Soak 60 s Volllast bei 24 MHz / 680 mV: 0 Rechenfehler. Schnellwechsel ueber die
  Tief-Takte (309 Spruenge): 0 Fehler.
- **Undervolt muss frequenzabhaengig sein:** die ganze Tabelle (bis 2.4 GHz) mit
  680 mV laufen zu lassen haengt das Board (2.4 GHz braucht 1040 mV) -> Watchdog-
  Reboot. Nur die Tief-Takte absenken.
- **Testfalle:** der Hash haengt von (Seed, Iterationen) ab; die Iterationszahl
  wird je Takt gewaehlt. Eine globale Referenz erzeugt falsche Fehler. Referenz
  je Takt bei Werksspannung messen.
- **Persistenz:** nur ueber die OPP-Tabelle im Overlay
  (`gen_lowfreq_dts.py --big-mv/--little-mv`, lehnt < 680 mV ab). Laufzeit-UV
  haelt nicht. Boot-Loop-Risiko, weil schedutil die Tief-Takte gleich nach dem
  Boot erreicht -> Reserve lassen (Empfehlung >= 700 mV).
- `uv.py revert()` setzte frueher `scaling_min_freq` hart auf 1000000 und haette
  die persistente 15,6-MHz-Untergrenze geloescht; liest sie jetzt aus
  `/etc/default/cpu-freq-guard`.
- Dateien: remote-shell/ha/{uv_lowfreq.py,uv.py,gen_lowfreq_dts.py,
  test_gen_lowfreq_dts.py} + LOWFREQ-README.md, UV-README.md.


### Hotplug bis auf 1 Kern + Monitor-/Takt-Korrektur (2026-10-17)
- `cpu_hotplug.py`: Stufen neu **<0.5 -> 1 Kern**, <1.5 -> 2, <3 -> 4, sonst alle;
  `MIN_ONLINE = 1`. cpu0 bleibt immer online. Host-Test 134 Checks gruen.
  Auf dem Board verifiziert (Journal): `Last 0.36 -> online [0]`,
  `Last 0.53 -> online [0, 1]`. Ein-Kern-Betrieb mit HA+Docker stabil.
- **Monitor-Mysterium geloest:** das echte Taktsignal ist `cpu_clk` (little,
  Consumer `cpu0`) bzw. `cpub_clk` (big, Consumer `cpu2`). **Nicht**
  `cpu_clk_dyn`/`cpub_clk_dyn` - das ist der dvfs-Zweig des System-Controllers und
  traegt einen **veralteten** Wert. Frueher stand in der Doku das falsche Signal.
- `armbianmonitor -m` liest `scaling_cur_freq`. Fuer **alle 50 Raten** beider
  Cluster stimmt das mit dem echten Signal innerhalb **<1 %** (max 0,79 %).
  Der Monitor zeigt also den echten Takt; der vermutete Faktor-2-Fehler war der
  Vergleich mit `*_clk_dyn`.
- **Grenztest `floor2.py`:** alle 30 Punkte bis 1 GHz auf `duty 0` (Regler-Boden
  680 mV) -> 0 Rechenfehler. Der Boden ist stabil; Grenze ist der Regler, nicht
  das Silizium.
- **Fallstricke (real passiert):**
  1. **Spannung nie vor der Frequenz senken.** Erster Versuch schrieb `duty 0`
     waehrend schedutil noch bei 2.4 GHz stand -> Board haengt, Watchdog-Reboot.
     Erst `userspace` + Ziel-Frequenz, dann Spannung.
  2. **ext4 `/` mit `commit=120`.** Nach einem Hard-Reset gehen bis zu 120 s
     ungesyncte Schreibzugriffe verloren - ein per `install` deploytes Skript war
     danach wieder alt. Nach jedem Deploy `sync` (in deploy_*.sh ergaenzt).
- **Reboots 2026-10-17 (16:33, 17:00, 17:16):** 16:33 = Testfehler (duty 0 bei
  2.4 GHz). `watchdog bootstatus` ist immer 0 (Amlogic), `/var/log` ist zram ->
  Journal je Reboot weg, kein Reset-Grund im Log. `dmesg` zeigt
  `meson_clk_pll_set_rate: pll sys1_pll_dco didn't lock` / `sys_pll_dco didn't
  lock` bei Taktwechseln - Verdacht: PLL-Lock unter schnellen/extremen
  Frequenzwechseln (Testlast), nicht der Regler. Nicht abschliessend belegt.
- Dateien: remote-shell/ha/{cpu_hotplug.py,test_cpu_hotplug.py,deploy_hotplug.sh,
  deploy_lowfreq.sh} + LOWFREQ-README.md.



### Boot-Journal: Reset-Zaehler mit Grund (2026-10-17)
- **Problem:** Amlogic liefert keinen Reset-Grund - `watchdog0/bootstatus`=0
  (immer), pstore leer, `/var/log`=zram. Das Journal ist persistent
  (`/var/log/journal -> /var/log.hdd/journal`), bricht aber bei jedem Hard-Reset
  ab (kein "Reached target Shutdown"). Von 20 Boots hatte **keiner** einen
  Clean-Marker -> alle waren Hard-Resets.
- **Loesung:** `boot_journal.py` + `boot_journal_lib.py` (host-getestet, 22
  Checks) zaehlen Boots persistent und leiten den Grund ab: Clean-Marker
  (`ExecStop` von boot-journal.service) -> clean-reboot; watchdog bootstatus ->
  watchdog-reset; "Kernel panic" im prev-Log -> kernel-panic; prev-Lauf <60s ->
  early-reset; sonst hard-reset-unknown. Zustand in /var/lib/boot-journal/.
- **Board-verifiziert:** sauberer Reboot -> `clean-reboot` (prev_uptime exakt =
  letzter Heartbeat). Kernel-Panic (sysrq-trigger, panic=10) -> `early-reset`.
- **Grenze (ehrlich):** ein Panic schreibt sich NICHT ins Journal (journald
  flusht nicht vor dem Freeze). Diese Luecke schliesst jetzt ramoops/pstore
  (siehe naechster Abschnitt) - die primaere Panic-Spur ist pstore.
- Dateien: remote-shell/ha/{boot_journal.py,boot_journal_lib.py,
  test_boot_journal_lib.py,test_boot_journal_flow.py,boot-journal*.{service,timer},
  deploy_boot_journal.sh} + BOOT-JOURNAL-README.md.

### ramoops/pstore: Kernel-Panics ueberleben den Warm-Reset (2026-10-17)
- **Ziel:** Panics ueberleben den Reset, damit boot_journal den echten Grund
  (`kernel-panic`) statt nur `early-reset`/`hard-reset-unknown` melden kann.
- **Loesung:** DT-Overlay `ramoops` reserviert 1 MiB RAM bei **0x50000000** (im
  freien Fenster 0x40000000-0xdfd2dfff, NICHT mit secmon 0x05000000 verwechseln).
  `gen_ramoops_dts.py` erzeugt es, `deploy_ramoops.sh` installiert es
  (`user_overlays=lowfreq ramoops`), `revert_ramoops.sh` nimmt es zurueck.
  Der Kernel nutzt die Region als pstore; `systemd-pstore` archiviert den Dump
  nach /var/lib/systemd/pstore/. `boot_journal.py` liest beide Orte.
- **Dedupe (wichtig):** ramoops/systemd-pstore behalten ALTE Dumps. boot_journal
  merkt sich die mtime des verbrauchten Dumps MONOTON (`consumed_pstore_mtime`)
  und ignoriert alles, was nicht juenger ist - sonst klebt ein alter Panic an
  jedem weiteren Boot. (Real passiert: `dmesg.txt` aelter + `dmesg-ramoops-0`
  neuer -> der aeltere ueberschrieb die Marke. Fix = `mtime <= exclude`.)
- **Board-verifiziert:** dmesg `reserved mem ... ramoops@50000000 (1024 KiB)`,
  ramoops-Modul geladen, Panic (sysrq-trigger) -> dmesg-ramoops-0 enthaelt
  `Kernel panic - not syncing: sysrq triggered crash` + Call trace -> Boot wird
  als `kernel-panic` MIT Text klassifiziert. Danach zwei saubere Reboots ->
  `clean-reboot` ohne Text. **Nur Warm-Reset** (Kaltstart loescht RAM).
- **Brick-Sicherheit:** boot.cmd stellt bei `fdt apply`-Fehler den Original-DT
  her -> ein kaputtes Overlay kann das Board nicht bricken.
- Dateien: remote-shell/ha/{gen_ramoops_dts.py,ramoops.dts,deploy_ramoops.sh,
  revert_ramoops.sh,test_gen_ramoops_dts.py,test_boot_journal_pstore.py,
  test_boot_journal_flow.py} + RAMOOPS-README.md.
