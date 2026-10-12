# To-do – esp32-IR

Stand: Branch `cline/power-history-diag` (Basis: On-Demand-Diagnose + Leistungshistorie).

## ⚠️ Verbindliche Regel: SPIFFS-Historie muss erhalten bleiben

Die persistente Leistungshistorie liegt in der SPIFFS-Partition `storage`
(Offset **`0x320000`**, Größe **`0xE0000`** = 917504 B, Datei `/powerlog/quarters.bin`,
bis zu 30000 Minutenwerte).

**Diese Daten dürfen NIE verloren gehen.** Konkret einzuhalten:

- **`storage`-Offset und -Größe niemals ändern** und niemals verschieben — sonst ist
  der bestehende Verlauf nach einem Flash weg.
- **Flash-Bundles/`flasher_args.json` dürfen die `storage`-Partition nicht schreiben.**
  (bereits umgesetzt: `e944e04`/`aac6ee0`; Bundle enthält keinen `storage`-Eintrag und
  keine `storage.bin`.) Vor jeder Bundle-Änderung erneut prüfen.
- **Kein `erase-flash` / kein Full-Chip-Erase** beim Deployment — nur
  bootloader + partition-table + app schreiben, `storage` bleibt unberührt.
- Bei jeder Partitions-/OTA-/Build-/Flash-Änderung prüfen: **bleibt `storage` an exakt
  derselben Adresse?** Wenn nein, Änderung verwerfen oder Verlust ausdrücklich absprechen.
- Nach jedem Deployment am Gerät verifizieren: **Verlauf noch vorhanden**
  (`record_count` > 0, nicht RAM-Fallback) — Akzeptanzkriterium, kein „nice to have".

## Offen

### Push / Zugang (blockiert)
- [ ] **Fine-grained PAT (`github_pat_…`) als `GITHUB_WRITE_TOKEN` hinterlegen.**
      Aktueller Stand: `GITHUB_TOKEN` ist in dieser Umgebung ein **GitHub-App-Token**
      (`ghu_…`, kurzlebig) mit reinem **Lesezugriff**. Schreibversuche (Branch/Commit/Push)
      scheitern mit `403 Resource not accessible by integration`. Der frühere erfolgreiche
      Push lief über einen **PAT**. Benötigt: fine-grained Token mit *Contents: Read and write*
      (+ *Workflows: Read and write* bei Workflow-Änderungen) auf `christiansiebe/esp32-IR`,
      als Secret `GITHUB_WRITE_TOKEN` in OpenHands.
      → Solange offen: **kein Push möglich**, alle neuen Commits bleiben nur lokal.
- [ ] Nach Token-Erneuerung: Bergungs-Branch `recovered/ams-features-salvage` pushen.

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

## Partitionstabelle (geändert, Commit `c3d3420`)

Ziel: zwei echte OTA-Slots à 1,5 MB (für OTA-Updates ohne Kabel), **storage bleibt
an exakt derselben Adresse**, damit die SPIFFS-Minutenhistorie einen COM-Re-Flash
überlebt.

**Vorher:**
```
nvs      data nvs     0x9000   0x6000
otadata  data ota     0xf000   0x2000
phy_init data phy     0x11000  0x1000
factory  app  factory 0x20000  0x100000
ota_0    app  ota_0   0x120000 0x100000
ota_1    app  ota_1   0x220000 0x100000
storage  data spiffs  0x320000 0xE0000
```

**Nachher:**
```
nvs      data nvs     0x9000   0x6000
otadata  data ota     0xf000   0x2000
phy_init data phy     0x11000  0x1000
ota_0    app  ota_0   0x20000  0x180000
ota_1    app  ota_1   0x1a0000 0x180000
storage  data spiffs  0x320000 0xE0000
```

Konkret geändert:
- **`factory`-App-Slot entfernt** (lag bei `0x20000`/`1 MB`).
- **`ota_0` vergrößert und verschoben:** `0x120000`/`0x100000` -> **`0x20000`/`0x180000`**
  (übernimmt den bisherigen Factory-Platz).
- **`ota_1` vergrößert und verschoben:** `0x220000`/`0x100000` -> **`0x1a0000`/`0x180000`**.
- **`storage` unverändert:** Offset `0x320000`, Größe `0xE0000` (= 917504 B) -> Historie bleibt.
  **Nicht verhandelbar** — siehe Regel oben („SPIFFS-Historie muss erhalten bleiben").
- `nvs`, `otadata`, `phy_init` unverändert.

Auswirkungen / Konsequenzen:
- **Flash-Ziel neu:** Die App wird jetzt nach `ota_0@0x20000` geflasht (vorher Factory
  `0x20000`). `otadata` entscheidet, welcher OTA-Slot bootet.
- **Ein Factory-Boot-Fallback existiert nicht mehr** — bei fehlgeschlagenem OTA greift
  stattdessen `esp_ota_mark_app_invalid_rollback_and_reboot()` (Rollback in den anderen
  OTA-Slot), abgesichert durch die Boot-Confirmation-Logik (siehe geborgenes
  `boot_confirm`/`boot_policy`).
- **`Firmware/Receiver/flasher_args.json` angepasst** (nur `0x1000`/`0x8000`/`0xf000`/`0x20000`),
  **storage-Eintrag entfernt** -> Flash-Bundles schreiben die Historie nie
  (Commits `e944e04`/`aac6ee0`).
- **Erstes Deployment nach dem Umbau erfordert Full-Flash** (neue Partitionstabelle +
  Bootloader/Partition-Table-Update); danach sind OTA-Updates möglich.

Offen / zu verifizieren:
- [ ] Gerät mit neuer `partitions.csv` **per Kabel komplett flashen** (bootloader + partition-table
      + app; `storage` bleibt unberührt) und booten prüfen.
- [ ] Danach einen **OTA-Update-Zyklus** testen (Accept + Rollback bei Fehlschlag).
- [ ] Prüfen, dass die Historie nach dem COM-Flash **erhalten** bleibt (SPIFFS-Historie > 0).

## Übergabe

Stand `/workspace/esp32-IR`, Branch `cline/power-history-diag`, **4 lokale Commits vor `origin`**
(**nicht gepusht**). Arbeitsbaum sauber. Backup: `/workspace/esp32-IR-backup-20261012.bundle`.

    aae37ec TODO: record push blocker, recovered AMS work and comparison follow-ups
    4af921a power_history: migrate old record formats instead of dropping them
    72004b7 docs: mark persistence migration done, note record-format versioning
    8764f22 tests: add power-history stability/fault-injection harness (ASan/UBSan)

**Push-Status (blockiert):** `GITHUB_TOKEN` -> HTTP 200, aber **nur Lesen** (GitHub-App-Token
`ghu_…`; Branch/Commit/Push scheitern mit `403 Resource not accessible by integration`).
`GITHUB_WRITE_TOKEN` -> **401 (ungültig)**. Früherer erfolgreicher Push lief über einen
fine-grained PAT.

**1. Schritt in der neuen Session — Token + Push:**
```
# erwartet 200
curl -s -o /dev/null -w "%{http_code}\n" -H "Authorization: Bearer $GITHUB_TOKEN" \
  https://api.github.com/repos/christiansiebe/esp32-IR
# Schreibtest (Branch anlegen + löschen); 403 = kein Schreibrecht
curl -s -X POST -H "Authorization: Bearer $GITHUB_TOKEN" \
  https://api.github.com/repos/christiansiebe/esp32-IR/git/refs \
  -d "{\"ref\":\"refs/heads/probe-\$\$\",\"sha\":\"<main-sha>\"}"
cd /workspace/esp32-IR && git push origin cline/power-history-diag
```
Wenn der Push erneut 403/401 gibt: **schreibfähigen fine-grained PAT (`github_pat_…`,
*Contents: Read and write*) als `GITHUB_WRITE_TOKEN` hinterlegen** und mit diesem pushen
(`git remote set-url origin https://x-access-token:$GITHUB_WRITE_TOKEN@github.com/christiansiebe/esp32-IR.git`).
Danach: Bergungs-Branch `recovered/ams-features-salvage` pushen und CI-Ergebnis prüfen.

**2. Am Gerät:** Bundle flashen -> SPIFFS-Verlauf muss erhalten bleiben (Flash-Bundle schreibt
die `storage`-Partition bewusst nicht, siehe `e944e04`/`aac6ee0`).

**Tests (lokal, vor Push):** `./test/run_host_tests.sh` — alles grün, inkl. Stabilitäts-Harness
(`test/stability/`, 10 Fehlerinjektions-Tests, ASan+UBSan, 61 Checks, 0 Failures).

**Als Nächstes ohne Push möglich:** Web-Konsole (`main/web_console.c`) um `nvs dump`,
`sntp resync`, `espnow diag` erweitern (host-testbar, siehe Debug-Konsole unten).

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
