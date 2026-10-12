# To-do – esp32-IR

Stand: Branch `cline/power-history-diag` (Basis: On-Demand-Diagnose + Leistungshistorie).

## Offen

### ⚠️ Verbindliche Regel: SPIFFS-Historie muss erhalten bleiben (nicht verhandelbar)
Die persistente Leistungshistorie liegt in der SPIFFS-Partition `storage`
(Offset **0x320000**, Größe **0xE0000** = 917504 B, Datei `/powerlog/quarters.bin`,
bis zu 30000 Minutenwerte).

**Diese Daten dürfen NIE verloren gehen.** Konkret:
- `storage`-Offset und -Größe niemals ändern und niemals verschieben — sonst ist der
  bestehende Verlauf nach dem nächsten Flash weg. (Genau deshalb blieb `storage` bei
  der OTA-Neuaufteilung in `c3d3420` auf 0x320000/0xE0000.)
- Flash-Bundles/`flasher_args.json` dürfen die `storage`-Partition **nicht** schreiben
  (bereits umgesetzt: `e944e04`/`aac6ee0`; bei jeder Bundle-Änderung erneut prüfen).
- Kein `erase-flash`/Full-Chip-Erase beim Deployment — nur bootloader + partition-table
  + App schreiben; `storage` bleibt unberührt.
- Bei jeder Partitions-/OTA-/Build-/Flash-Änderung prüfen: bleibt `storage` an exakt
  derselben Adresse? Wenn nein: Änderung verwerfen oder Verlust ausdrücklich absprechen.
- Nach jedem Deployment am Gerät verifizieren: Verlauf vorhanden (`record_count > 0`,
  kein RAM-Fallback) — **Akzeptanzkriterium**, kein „nice to have".

### 🐛 Bug: `record_count` nach dem Boot nicht wiederhergestellt
Symptom: Nach einem Reboot wird die persistierte Minutenhistorie nicht (oder nicht
vollständig) geladen — `record_count` startet bei 0 statt beim gespeicherten Wert
(ggf. RAM-Fallback trotz vorhandener `/powerlog/quarters.bin`).

**Stand:** Der bekannte Auslöser (`1612975`, ungültiger Fresh-Header `magic=0`) ist
behoben und war laut `d620ea0` hardware-verifiziert. Der Speicherpfad war host-seitig
bisher **ungedeckt** (der Shim rief `power_history_init()` nie auf). Neu: Host-Test
`test/test_reboot_persistence.c` fährt den echten Init→Schreiben→Reboot-Zyklus (per
`fork()`) und belegt `record_count` wird wiederhergestellt — rot mit dem alten Bug,
grün mit dem Fix.

Zu prüfen / zu tun:
- [ ] Beim Boot verifizieren, dass `power_history` die SPIFFS-Datei öffnet und den
      Header liest (nicht in den RAM-Fallback fällt). Logzeile `Persistent history
      ready: N of 30000` gegen `record_count` in `/api/power-history` abgleichen.
- [ ] Header-Handling prüfen: Nach dem Fix `1612975` (gültiger Fresh-Header + In-Place-
      Reparatur) darf ein gültiger Bestand **nicht** als „leer" gewertet werden.
- [ ] Migration prüfen (`4af921a`): Alte `format_version` darf die Datensätze nicht
      mehr verwerfen — nach dem Boot müssen migrierte Datensätze `record_count > 0` ergeben.
- [ ] Akzeptanzkriterium (Pflicht): nach Reboot sofort `record_count > 0`, kein RAM-Fallback,
      0 Schreibfehler. Am Gerät verifizieren.

### Push / Zugang (blockiert)
- [ ] **Gültigen fine-grained PAT (`github_pat_…`) als `GITHUB_WRITE_TOKEN` hinterlegen.**
      Geprüfter Stand in dieser Session: `GITHUB_TOKEN` = GitHub-App-Token (`ghu_…`), read-only
      (`git push` -> `403 Permission to christiansiebe/esp32-IR.git denied`, auch auf bestehenden
      Branch). `GITHUB_WRITE_TOKEN` = 93 Zeichen, Prefix `githu…` -> **`401 Bad credentials`**
      (ungültig/abgelaufen), für API *und* Git. Ein Schreibzugang fehlt damit komplett.
      Benötigt: fine-grained Token mit *Contents: Read and write* (+ *Workflows: Read and write*
      bei Workflow-Änderungen) auf `christiansiebe/esp32-IR`.
      → Solange offen: alle neuen Commits bleiben nur lokal.
- [ ] Bergungs-Branch `recovered/ams-features-salvage` (lokal, Commit `d10ea8c`) pushen.

### Geborgene Arbeit (AMS-Features, aus verlorenem `/tmp/work` gerettet)
Kontext: In einer früheren Session lag die Arbeit in **`/tmp/work/`** (nicht `/workspace`).
`/tmp` wird beim Sandbox-Reset geleert → Arbeit war **nie committet** und damit **verloren**.
Aus dem Event-Log rekonstruiert und gesichert.
- [x] Rekonstruktion aus `/workspace/conversations/98b3432df79246699b316a09ee117d53`.
- [x] Lokal gesichert: Branch **`recovered/ams-features-salvage`** (Commit `d10ea8c`, `README.md`
      mit Herkunft/Grenzen) + Kopie unter `/workspace/project/.../recovered-ams/`.
- [ ] **Nach `main.c` integrieren** (nur teilweise nachspielbar, daher noch nicht baubar):
      - [ ] `health_policy.{c,h}`, `health_task.{c,h}` — Health-Monitor / selbstheilender Reboot + TWDT
      - [ ] `mqtt_publish.{c,h}`, `mqtt_bridge.{c,h}`, `mqtt_client.h` — MQTT + HA-Discovery
      - [ ] `boot_confirm.{c,h}`, `boot_policy.{c,h}` — Post-OTA-Rollback-Bestätigung
      - [ ] `recovery_button.{c,h}`, `recovery_start.{c,h}` — BOOT-Tasten-Recovery
      - [ ] `test/test_health_policy.c`, `test/test_mqtt_bridge.c`, `test/test_boot_policy.c`,
            `test/test_recovery_button.c` in `test/run_host_tests.sh` aufnehmen
      - [ ] `main/CMakeLists.txt`, `sdkconfig.defaults` (MQTT/esp-mqtt), `idf_component.yml`
- [ ] Offene Integrationsschritte, die **nicht** rekonstruierbar waren und neu gemacht werden müssen:
      `main/main.c`-Aufrufe, `power_history.c`-Migration (s. u.), `run_host_tests.sh`-Einträge.
- [ ] Nach Integration: beide Rollen über CI bauen (Nachweis fehlt bisher komplett).

### AMS-Reader-Vergleich (`amsreader-firmware`) — Übernahmen
Reihenfolge nach Wirkung/Aufwand. „besser gemacht" = konkrete Lehre aus AMS.
- [ ] **LICENSE + README** fehlen komplett (unabhängig, ~30 Min).
      Lizenz wählen (MIT/Apache für dieses Projekt); README mit Build-/Flash-Anleitung.
- [x] **Persistenz-Migration** (echte Lücke, **erledigt** `4af921a`): `record_is_valid()`
      verwarf `format_version != POWER_HISTORY_VERSION` hart -> bei Versionssprung wurden **alle**
      Altbestände ungültig und `read`/`aggregate` übersprangen sie still (Datenverlust nach Update).
      Neu: `power_history_migrate_record()` (in `power_history.h/.c`) hebt einen Datensatz in
      Ort und Stelle auf das aktuelle Format, toleriert Versionen in
      `[POWER_HISTORY_OLDEST_SUPPORTED_VERSION, POWER_HISTORY_VERSION]`, bleibt aber streng beim
      CRC (Formatwechsel != Korruption; nach der Migration wird die Checksumme neu berechnet).
      `read()` und `aggregate()` migrieren jetzt vor der Nutzung. Tests:
      `test_record_migration` + `test_migration_in_aggregation` (11 Checks). Ohne Fix rot
      (v1-Datensatz wird verworfen), mit Fix grün - rot/grün verifiziert. **Noch nicht gepusht.**
- [ ] **Health-Reboot / Selbstheilung**: Reboot, wenn z. B. `last_rx_ago_ms` > N Minuten oder
      Wi-Fi down > N Minuten. Nutzt vorhandenes `last_rx_ago_ms`. (Im geretteten
      `health_*`-Modul angelegt, aber nicht integriert.)
- [ ] **TWDT explizit**: Worker (`power_history_worker`, `uart_event_worker`) beim
      Task-Watchdog anmelden (`esp_task_wdt_init(120,true); esp_task_wdt_add(...)`) —
      statt IDF-Default. Aktuell **0 Treffer** für `esp_task_wdt`. (In `health_*` angelegt.)
- [x] **Post-OTA-Rollback-Bestätigung, Empfänger** (integriert): `boot_policy.c/.h`
      (reine, host-testbare Entscheidung) + `boot_confirm.c/.h` (Kleber) ersetzen das
      unbedingte `esp_ota_mark_app_valid_cancel_rollback()` im Empfänger. Bestätigung erst
      nach **45 s** stabil + Dashboard oben; kommt das Dashboard nach **30 s** Grace nicht
      hoch → Rollback in den vorigen OTA-Slot. Sender bewusst unverändert (hat kein
      Dashboard) — eigener Confirm-Pfad für den Sender ist offen. Tests:
      `test/test_boot_policy.c` (10 Checks, rot/grün verifiziert), im Runner aktiv.
      **⚠️ Hardware-Verifikation offen:** der echte OTA→bestätigt / OTA→Rollback-Pfad ist
      am Gerät zu prüfen (host-seitig nur die Entscheidungslogik abgedeckt).
- [ ] **SML-Golden-Fixtures**: `test/fixtures/sml/*.bin` mit echten Zähler-Telegrammen +
      Decode-Golden-Test (bester Regressionsschutz gegen Parser-Bugs).
- [ ] **AP abschalten, wenn STA verbunden** (größter einzelner Spareffekt): Empfänger fährt
      dauerhaft `WIFI_MODE_APSTA` (`main.c`); nach `GOT_IP` auf `WIFI_MODE_STA` wechseln.
- [ ] **Tages-/Monats-Summen im Flash** (`hImport[24]`/`dImport[31]` wie AMS): aus
      Minutenwerten ableiten, platzsparend + schnellere Langzeitgrafik.
- [ ] **Letzte bekannte Zeit persistieren** (`lastMeterReadTime`): nach Reboot ohne SNTP ist
      `bucket_start==0` → keine Aufzeichnung; letzte UTC im Header/NVS ablegen.
- [ ] **MQTT-Availability (LWT)** + Service-Events (Reboot-Grund) — ergänzt Crash-Journal.
      (In `mqtt_*` angelegt, aber nicht integriert.)
- [ ] Optional: konfigurierbares Reboot-Intervall (nächtlich), Deckelung wie AMS.

### SPIFFS-Historie auch im Sender (gleiche Speicherung wie Empfänger)
Ziel: Der **Sender** soll ebenfalls die persistente Minutenhistorie bekommen und die Daten
**genau wie der Empfänger** ablegen (gleiches `power_history`-Format, gleiche Datei).
- Ausgangslage: `power_history_init()`, `g_power_sample_timer` und der History-Worker laufen
  nur im Empfänger-Zweig (`app_main`, `#else`-Block zu `CONFIG_ESPNOW_ROLE_SENDER`,
  `main.c` um 4206–4216). Der Sender sendet nur per ESP-NOW, führt **keine** lokale Historie.
- Partition ist bereits vorhanden: `partitions.csv` ist für **beide** Rollen identisch,
  `storage` = `data/spiffs`, `0x320000`/`0xE0000` liegt auch im Sender-Image. Also **keine**
  Repartitionierung nötig — dieselbe Datei `/powerlog/quarters.bin`, dieselben Offsets.
- Zu tun:
  - [ ] `power_history_init()` + Sample-Timer + Worker aus dem Empfänger-`#else` herauslösen,
        sodass **beide** Rollen sie starten (Sender: keine Web-API/Aggregation nötig, nur
        Aufzeichnung + Persistenz).
  - [ ] Gleiches On-Disk-Format/Datei wie Empfänger (nichts am Layout ändern, siehe
        Non-negotiable „Historie nie verlieren"), Migration/Versionen (`4af921a`) gelten mit.
  - [ ] Sender-Zeitbasis: ohne Empfänger gibt es keine Wall-Clock (Uptime-Fallback) → Sender-
        Historie muss `bucket_start==0` sauber behandeln (hängt an „letzte Zeit persistieren").
  - [ ] Flash-Regel bleibt: `storage` bei **keinem** Bundle schreiben/erassen; nach jedem
        Flash `record_count > 0` prüfen (auch beim Sender).
  - [ ] Host-Test + CI: beide Rollen bauen; Persistenz-Test
        (`test/test_reboot_persistence.c`) gilt analog für den Sender.
- Akzeptanz: Sender legt die Minutenhistorie im Flash ab und stellt sie nach Reboot wieder her
  (`record_count > 0`, kein RAM-Fallback) — identisch zum Empfänger.

## Partitionstabelle (geändert, `c3d3420`) — Flash-Reihenfolge & Auswirkungen
Ziel: zwei echte OTA-Slots à 1,5 MB; `storage` bleibt an exakt derselben Adresse, damit
die SPIFFS-Minutenhistorie einen COM-Re-Flash überlebt.

Vorher:
```
factory  app  factory 0x20000  0x100000
ota_0    app  ota_0   0x120000 0x100000
ota_1    app  ota_1   0x220000 0x100000
storage  data spiffs  0x320000 0xE0000
```
Nachher:
```
ota_0    app  ota_0   0x20000  0x180000
ota_1    app  ota_1   0x1a0000 0x180000
storage  data spiffs  0x320000 0xE0000
```
Konkret geändert:
- `factory`-App-Slot **entfernt** (war 0x20000/1 MB).
- `ota_0`: 0x120000/0x100000 → **0x20000/0x180000**.
- `ota_1`: 0x220000/0x100000 → **0x1a0000/0x180000**.
- `storage` **unverändert** (0x320000/0xE0000) → Historie bleibt. Nicht verhandelbar
  (siehe Regel oben).
- `nvs`/`otadata`/`phy_init` unverändert.

Auswirkungen:
- Die App wird jetzt nach **`ota_0@0x20000`** geflasht (vorher `factory@0x20000`);
  `otadata` entscheidet den Boot-Slot.
- Kein Factory-Fallback mehr; Rollback in den **anderen** OTA-Slot via
  `esp_ota_mark_app_invalid_rollback_and_reboot()` (Boot-Confirmation, s. AMS-Punkt).
- `Firmware/Receiver/flasher_args.json` ohne `storage`-Eintrag → Bundle schreibt die
  Historie nie (`e944e04`).
- **Erstes Deployment = Full-Flash** (bootloader + partition-table + App), `storage`
  bleibt unberührt.

Flash-Reihenfolge (USB-Full-Flash, Empfänger):
```
0x1000   bootloader.bin
0x8000   partition-table.bin
0xf000   ota_data_initial.bin
0x20000  receiver-sml.bin      # App jetzt in ota_0, nicht mehr factory
# storage NICHT flashen (Historie erhalten)
```

Offen / zu verifizieren:
- [ ] Gerät per Kabel komplett flashen (`storage` unberührt), Boot prüfen.
- [ ] OTA-Update-Zyklus testen (Accept + Rollback).
- [ ] Historie nach COM-Flash erhalten (`record_count > 0`).

## Übergabe

Stand: Branch `cline/power-history-diag`, `origin` bei `3f58fec`. **Lokale Commits vor
`origin` (nicht gepusht, kein Schreibzugang).** Die früher gelisteten 4 Commits
(`aae37ec`, `4af921a`, `72004b7`, `8764f22`) sind inzwischen **gepusht**; seitdem kamen
lokal weitere Commits hinzu (TODO-Dokumentation zu Bug/Partition/Regel).

**Push-Status (blockiert, geprüft):** `GITHUB_TOKEN` = GitHub-App-Token (`ghu_…`),
read-only -> `403 Permission denied` beim `git push`. `GITHUB_WRITE_TOKEN` = 93 Zeichen
(`githu…`) -> `401 Bad credentials` (ungültig). Ohne gültigen fine-grained PAT bleibt
alles lokal.

**Offen in dieser Session:** siehe die beiden Blöcke oben
(Bug `record_count` + Partitionstabelle/Flash-Reihenfolge).

**2. Am Gerät:** Nach dem Flash der neuen Partitionstabelle (`c3d3420`) ist ein
**Full-Flash** nötig (bootloader + partition-table + App nach `ota_0@0x20000`). Die
`storage`-Partition **nicht** flashen — der SPIFFS-Verlauf muss erhalten bleiben
(Bundle schreibt `storage` bewusst nicht, siehe `e944e04`/`aac6ee0`). Danach prüfen:
`record_count > 0`, kein RAM-Fallback.

**Tests (lokal, vor Push):** `./test/run_host_tests.sh` — alles grün, inkl. Stabilitäts-Harness
(`test/stability/`, 10 Fehlerinjektions-Tests, ASan+UBSan, 61 Checks, 0 Failures).

**Als Nächstes:** Web-Konsole (`main/web_console.c`) um `nvs dump`, `sntp resync`,
`espnow diag` erweitern (host-testbar, siehe Debug-Konsole unten).

### Sender-Diagnose (On-Demand über ESP-NOW)
- [ ] **Event-Zeitstempel als Uptime darstellen** statt als 1970-Datum.
      Grund: Der Sender hat keine Netzwerkzeit (SNTP laeuft nur im Empfaenger);
      `timestamp_ms` ist `esp_timer_get_time()/1000`.
      Betroffen: `main/main.c` -> `diag_page_handler` (JS rendert `new Date(e.timestamp_ms).toISOString()`).
- [ ] **Sender->Empfaenger-RSSI im Diagnose-Report ausweisen** (Funkqualitaet der Nutztelemetrie).
      Quelle: Der Empfaenger kennt sie bereits im `receive_callback` (`info->rx_ctrl->rssi`);
      bisher wird nur die Richtung Empfaenger->Sender als `last_request_rssi` uebertragen.

### CI / Build
- [x] **`storage.bin` in das CI-Flash-Artefakt aufnehmen** — erledigt (`71f5ea8`), CI-Staging
      prueft und kopiert `storage.bin` jetzt; im Run `38141912733` verifiziert.
- [x] **CPU-Takt fest auf 80 MHz** statt IDF-Default 160 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80=y`
      in `sdkconfig.defaults`). Das Geraeat ist netzwerk-/IO-gebunden (SML-UART 9600 Baud), der
      doppelte Takt bringt kaum etwas, kostet aber ungefaehr doppelt so viel dynamischen Strom.
      Bewusst **fest** statt DFS: WiFi haelt ohnehin einen `ESP_PM_APB_FREQ_MAX`-Lock (DFS landet
      also auch bei 80 MHz), und auf dem ESP32 muss DFS fuer zuverlaessiges ESP-NOW deaktiviert
      bleiben. CI prueft `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=80` in beiden Rollen.

### Bedienung / Diagnose (neu)
- [ ] **Reboot-Taste im Dashboard** (`POST /api/reboot`, Auth-Gate wie `/update`,
      Antwort senden, dann `esp_restart()`). Nuetzlich fuer den Persistenz-Test
      (Reboot ohne Stecker ziehen) und generell zum Neustart aus der Weboberflaeche.
      Muss auf der AP-Seite auch ohne STA-Verbindung funktionieren.
      **Teilweise erledigt:** die Web-Konsole (`web_console.c`, `/api/console`) hat
      bereits den Befehl `reboot` (`console_cmd_reboot` -> `esp_restart()`); es fehlt
      nur noch die prominente Taste im Dashboard.
- [~] **Debug-Konsole mit Eingabe** — **teilweise erledigt:** Web-Konsole existiert
      (`/api/console`, `web_console.c`) mit Befehlen `help`, `status`, `history`,
      `time`, `wifi`, `power`, `reboot` (Registrierung `web_console_register_commands`).
      Offen: weitere Befehle (`nvs dump`, `sntp resync`, `espnow diag`) und/oder eine
      **serielle** Eingabe (UART ist durch SML-Eingang belegt). Beruehrt keine
      geschuetzten Bereiche.

### Persistenz / Zeit
- [ ] **Minutenwerte erst nach NTP-Sync** (`wall_clock >= 1704067200`): Vor der
      Zeitsynchronisation werden zwar Samples gezaehlt, aber kein Minutenwert
      abgeschlossen (`record_count = 0`). Kandidat: relative Boot-Zeitbasis mit
      spaeterem Nachziehen der Zeitstempel. Beruehrt die `day=…`-UTC-Semantik ->
      mit Nutzer abstimmen.
- [ ] **`storage_ready` praezisieren**: `status->ready` ist auch im RAM-Fallback
      `true` (nur bei fehlgeschlagenem Task-Start `false`). Verlaesslich ist bisher
      nur die Boot-Log-Zeile `Persistent history ready: N of 30000`. Echten
      SPIFFS-Status (Datei offen) melden, damit „bereit" nicht in die Irre fuehrt.

### MQTT / Home Assistant (geplant)
- [x] **Speicherdaten nach HA bringen** (ohne MQTT): `tools/ha_import.py` zieht die
      Rohdaten ueber `/api/power-history?after=<seq>`, aggregiert stuendlich
      (`tools/ha_stats.py`) und importiert sie per HA-WebSocket-Befehl
      `recorder/import_statistics` mit echten UTC-Zeitstempeln. Version-aware
      (`mean_type`/`unit_class` ab HA 2025.10). Stdlib-only WebSocket-Client
      (`tools/ws_min.py`). Tests: 28 + 16 + 7 Checks, keine Mocks.
- [ ] **MQTT-Client im Empfaenger** (esp-mqtt; in IDF v6.1 als Komponente
      `espressif/mqtt` nachzuziehen). Broker/Port/User/Passwort + Topic-Prefix in
      NVS. LWT fuer online/offline, Publish von:
      - Live-Leistung: `<prefix>/power` (W)
      - Abgeschlossener Minutenwert: `<prefix>/history` als JSON
        `{"t":<utc>,"w":<avg>,"n":<count>}`
      - Verfuegbarkeit: `<prefix>/status` (online/offline via LWT)
      Nur Empfaenger (hat STA/Netz); ESP-NOW, OTA und Rollen bleiben unberuehrt.
- [ ] **Home-Assistant-Discovery** (`homeassistant/sensor/.../config`), damit die
      Entitaeten automatisch erscheinen: `device_class: power`,
      `state_class: measurement`, Unit W, plus Energie-Zaehler (kWh).
- [ ] Optional: Live-MQTT-Push der Minutenwerte, damit HA ab sofort selbst
      mitschreibt (dann ist der Import nur noch fuer die Alt-Daten noetig).

### Power-History-Dashboard (kosmetisch, optional)
- [x] **Balken-Wert per Hover/Tap einblenden**: Tooltip auf dem Canvas zeigt Wert + Zeitpunkt;
      Maus (`mousemove`) und Touch (`touchstart`/`touchmove`) unterstuetzt, funktioniert also
      auch am Touch-Display. Basis-Chart liegt in einem Offscreen-Canvas, beim Hervorheben
      wird nur geblittet + der Balken betont. JS-Syntax- und Laufzeit-Check in CI.
- [ ] `current_sample_count`-Label praezisieren (aktuell: 15-min-Ringpuffer, nicht Samples im aktuellen 60-s-Bucket).
- [ ] `resize`-Handler startet zusaetzliche 60-s-Schleifen (`updateHistory`) -> Mehrfach-Timer vermeiden.

### 🐛 Bug: Button „Vortag vergleichen“ tut in der Live-Ansicht nichts
Betrifft die **Empfänger-Weboberfläche** (Dashboard, `root_handler`/`sml_page` in
`main/main.c`). Der Sender hat keinen Webserver.

Symptom: Der Button `history-day-compare` („Vortag vergleichen“, `toggleCompare()`)
reagiert nicht, wenn die Grafik im **Live-Modus** steht (Default).

Ursache: `toggleCompare()` setzt nur `historyCompare`, wechselt aber **nicht** aus der
Live-Ansicht. In `updateHistory()` wird der Vergleich nur geladen, wenn `!live`:
```js
const live=(historyView==='live');
...
if(!live&&historyCompare){ /* Vortag laden */ }
```
Im Live-Modus bleibt der Button daher wirkungslos; zusätzlich wird die Info-Zeile
gerade im Live-Modus geleert (`live?'':...`), sodass auch kein Hinweis erscheint.

Zu tun:
- [ ] **Fix**: in `toggleCompare()` vor `redraw()` in die Tagesansicht wechseln
      (z. B. `setLive(false);`), dann wird der Vortag (gelb) über den Tagesverlauf gelegt.
- [ ] Optional: beim Wechsel in die Live-Ansicht `historyCompare` zurücksetzen
      (der Slider-Handler `history-range` tut das derzeit nicht).
- [ ] Verifizieren: Live → Button klicken → gestrichelte/gelbe Vergleichskurve +
      Info „Vergleich &lt;Datum&gt; (gelb)“ sichtbar.

## Erledigt (in dieser Session)
- On-Demand-Senderdiagnose ueber ESP-NOW (unicast, Request/Response, echte Sendebestaetigung via Callback).
- Persistente Leistungshistorie (SPIFFS/RAM-Fallback, SNTP, 1-s-Sampling, 60-s-Buckets, `/api/power-*`).
- SPIFFS-Robustheit: `format_if_mount_failed = true` + vorformatiertes Image (`spiffs_create_partition_image`).
- Hardware-Nachweis Diagnose (Sender): 2530/2530 Sendungen bestaetigt, 0 Drops, 0 UART-Fehler, 1 Diag-Request.
- **P2 – UTC-Tagesansicht + hostgetestete Aggregation** (`adc6154`): 15-min-Buckets aus
  60-s-Saetzen, Luecken werden nicht als 0 W gefuellt, Tag-Auswahl als UTC-Tag
  (`timegm`, DST-neutral), Host-Tests 36/36 gruen, beide CI-Rollen gruen.
- **Reboot-Persistenz-Bug behoben** (`1612975`): Die Leerdatei-Verzweigung schrieb einen
  ungueltigen Header (`magic = 0`) in beide Kopien, sodass jeder Boot in den RAM-Fallback
  fiel und der Verlauf auf 0 zurueckging. Fix: gueltiger Fresh-Header + In-Place-Reparatur
  bei ungueltigen Kopien. **Hardware-verifiziert**: nach Reboot sofort 10 Minutenwerte
  wieder vorhanden, `storage_ready: true`, 0 Schreibfehler.
- **Absturzdiagnose + persistentes Crash-Journal (Sender)**: Der Sender hat kein Netz, das
  On-Demand-Diagnose-Ringpuffer lag nur im RAM und war nach einem Absturz weg. Neu:
  - `main/crash_journal.{h,c}` (reine, hosttestbare Logik) + `main/crash_journal_store.c`
    (NVS-Bindung). Ein kleines, per CRC32 gesichertes Abbild (Zaehler + Schwanz des
    Event-Rings) ueberlebt den Reset.
  - Beim Boot wird der Reset-Grund geloggt und die vorige Session bewertet
    (`clean_shutdown`), inklusive der letzten Events. Ein Shutdown-Handler markiert
    saubere Reboots, damit Abstuerze erkennbar sind.
  - Neuer Diagnose-Abschnitt `crash` (Request/Response), Empfaenger liefert JSON und
    zeigt ihn auf `/diag` an. Antwort passt in ein ESP-NOW-Paket (243 Byte, Static-Assert).
  - Gestufte Schreibdrosselung gegen NVS-Verschleiss (ERROR 5 s, INFO/WARN 60 s)
    plus 60-s-Heartbeat, damit die Zaehler auch ohne Event aktuell bleiben.
  - **Parser-Haerten**: Off-by-one in `smlNewList` (`nodes[10]`), das bei tiefer
    Listenverschachtelung ein Element ueber das Array schrieb; dazu gepruefte
    OBIS-Varianten (`smlOBISCheck`, `smlOBISByUnitChecked`). Host-Test
    `test/test_sml_parser.cpp` laeuft unter AddressSanitizer und reproduziert den
    urspruenglichen Ueberlauf nachweislich (rot ohne Fix, gruen mit Fix).

## Code-Historie (chronologisch, neueste zuerst)

Kurz-Changelog der funktionalen Aenderungen, abgeleitet aus der Git-Historie
(Commit-Kuerzel in Klammern). Reine Doku-/Format-Commits sind zusammengefasst.

### 2026-10-12 – Infrastruktur, Robustheit, Sender
- `c3d3420` **Partition-Layout geaendert**: `factory` entfernt, 2x 1,5 MB OTA
  (`ota_0` @0x20000, `ota_1` @0x1A0000); `storage` bleibt @0x320000/0xE0000.
  -> einmalige **Flash-Migration** (Partitionstabelle + otadata + App) noetig.
- `e944e04` / `aac6ee0` Flash-Bundles schreiben die `storage`-Partition **nie**
  (SPIFFS-Verlauf bleibt erhalten).
- `a8a3e23` / `5a21fba` / `9c6deed` **ESP-NOW-Wall-Clock-Sync**: Sender holt die Zeit
  vom Empfaenger (behebt 1970-Zeitstempel in `/diag`).
- `bf3f0fd` / `81a2af9` **Sender-Selbstheilung** bei UART-Desync (auch wenn Bytes
  fliessen, aber nie ein Telegramm fertig wird).
- `f29b758` Post-OTA-**Rollback-Bestaetigung** statt Blind-Akzept (Empfaenger).
- `4af921a` `power_history`: alte Record-Formate **migrieren** statt verwerfen.
- `8764f22` Stability-/Fault-Injection-Harness (ASan/UBSan); `de3cec8` Reboot-Persistenz-Test.
- `6a52fe6` / `f736672` `tools/build.sh` (lokaler, cachebarer Docker-Build).
- Doku: `AGENTS.md`, `HANDOFF.md`, diverse `TODO.md`-Commits.

### 2026-10-11 – Dashboard/UI, Diagnose, Persistenz
- `3c8a669` **Chart-Zeitfenster als Slider** (Index -> Stunden).
- `adc6154` **UTC-Tagesansicht** + hostgetestete Aggregation (Luecken nicht als 0 W).
- `13a01ed` **Hover/Tap**: Wert + Zeitpunkt am Balken (Maus + Touch).
- `50585ea` **Browser-Debug-Konsole** (`/console`, `/api/console`).
- `85275f3` / `1b9dad2` Dashboard-Fixes (Stack-Overflow, Ein-Pass-Aggregation).
- `1612975` **Reboot-Persistenz-Bug** behoben (ungueltiger Fresh-Header `magic=0`).
- `a37cdf3` / `ec921f3` **Persistentes Crash-Journal** + Parser-Haertung
  (`smlNewList` Off-by-one, gepruefte OBIS-Varianten).
- `b19239b` / `740ad0f` / `f19fc51` **UART-Gap-Messung** + korrekte Klassifizierung
  (Luecke nur bei Checksum-Fehler als Verlust).
- `40baea3` / `fa24b5e` / `c53b028` CPU fest auf **80 MHz** + CI-Assert.
- `71f5ea8` `storage.bin` in CI-Flash-Artefakte; `f68b4fc` HA-Long-Term-Statistics-Import (Tool).

### 2026-10-10 – Basis dieser Arbeit
- `abdb7d4` / `80fe66a` **On-Demand-Senderdiagnose** ueber ESP-NOW (unicast,
  Request/Response, echte Sendebestaetigung, rollenscharfe Klassifizierer).
- `a201f5a` **Persistente Leistungshistorie** im Empfaenger (SPIFFS/RAM, SNTP,
  1-s-Sampling, 60-s-Buckets, `/api/power-samples|power-history|power-history.csv`).
- `7606a36` SPIFFS: `format_if_mount_failed = true` (kein Mount-Fehler mehr).
- `e712ac3` Vorformatiertes SPIFFS-Image (`spiffs_create_partition_image`).
- `6213c3f` TODO angelegt; `4917ed6` Receiver-Firmware-Snapshot.

### 2026-10-08 / 09 – Ursprung
- `31f9f99` Initiales ESP-IDF-Projekt + Cloud-Build.
- `4019d44` CI-Umgebung (ESP-IDF v6.1).
- `6589f35` SML-Werte auf der Startseite.
- `05ed7b9` Rohstream -> **SML-Telemetrie** (Transportpaket-Format).
- `4ffdcf0` SML-Parser-Abhaengigkeit deklariert (Referenz-Commit).
- `5035efc` / `44b6fe0` / `e5fdec1` Firmware-Staging/Upload im Workflow.
- `664e927` / `0070840` Artefakt-Pfade + `ota_data_initial.bin` im Build.
