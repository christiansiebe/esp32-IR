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
