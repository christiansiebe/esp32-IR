# To-do – esp32-IR

## NEU (2026-10-17): Laufzeit-Knöpfe überleben Reboot + OTA (NVS)
**Bisher waren `set cpu|freq|cpu-min|sleep|retries` RAM-only** — ein Reboot warf
sie auf die Compile-Time-Defaults zurück, also musste man zum dauerhaften Ändern
des Takts doch flashen. Jetzt liegen sie in NVS (Namespace `rtcfg`, versionierter
Blob):
- `set …` schreibt den Blob; `reset-config` löscht ihn.
- `runtime_config_load()` liest ihn in `active_runtime_config()` und klemmt ihn
  mit `runtime_config_clamp()` auf den Decken-Wert des laufenden Builds — ein
  Image mit kleinerem `CONFIG_ESPNOW_CPU_MAX_MHZ` kann keinen zu hohen Takt
  wiederherstellen.
- `apply_power_management()` wendet die geladene Config beim Boot an (Sender über
  `uart_sleep_set_pm_override`/`_set_enabled`, Empfänger direkt via `esp_pm_configure`).
- Host-Test `test/test_runtime_config.c` um die clamp-Fälle erweitert (36 Checks).
- [x] Am Empfänger verifiziert (`efa49fb`, 2026-10-17): `set freq 80` → `reboot`
      → `config` zeigt `cpu=80/80`, live `cpu_freq_mhz=80`; `reset-config` → wieder
      `240/40`. Und `set freq 80` → **OTA** auf dasselbe Image → nach dem
      OTA-Reboot weiterhin `cpu=80/80`. Backfill danach sauber
      (`backfill: 1 record merged`, `write errors: 0`, `queue drops: 0`).

## Empfänger „AP aus" (`feat/receiver-ap-off`, 2026-10-17) – PR #13
**Ergebnis: AP darf aus — aber nur über die Compile-Time-Option
`CONFIG_ESPNOW_RECEIVER_DISABLE_AP=y`, nie im laufenden Betrieb.**
- Ein **Laufzeit**-AP-Drop killte den ESP-NOW-Link (`04add16`, zurückgenommen in
  `71409c8`): die ESP-NOW-Peers des Empfängers liegen auf `WIFI_IF_AP`, ein
  abgerissenes AP-Interface stoppt jeden Austausch.
- Der Compile-Time-Weg schaltet `ESPNOW_IF` auf `WIFI_IF_STA` (`WIFI_MODE_STA`),
  es gibt gar kein AP-Interface zum Abreißen. Dashboard läuft über das Heim-WLAN
  (DHCP/mDNS).
- [x] CI-Matrix-Eintrag `receiver-apoff` baut das Image und prüft SoftAP aus +
      `ESPNOW_CHANNEL=1`.
- [x] Am Gerät geflasht (`db228f9`): Dashboard über WLAN (`192.168.178.70`),
      `/healthz` 200, SoftAP `ESP32-IR-Meter` weg, **ESP-NOW-Link lebt**
      (`caught_up: true`, `errors: 0`, `sender_mac_known: true`, Sender-Roundtrip
      ok, `sender_rssi_valid: true`), keine Daten verloren.
- Kanal-Zwang: ohne AP kann der Sender den Kanal nicht mehr entdecken; beide
  Seiten müssen denselben `CONFIG_ESPNOW_CHANNEL` (Default 1) nutzen, und da die
  Station an den Heim-AP-Kanal gebunden ist, muss dieser 1 sein (verifiziert:
  FRITZ!Box „Wohnzimmer" = Kanal 1).

## BEHOBEN (PR #14): Empfänger-`web_guard` startete den HTTP-Server im Leerlauf neu
**Ursache am Gerät belegt: vorzeichenloser Unterlauf in `web_guard_tick()`.**
Der Guard liest `now_ms` **vor** dem Selbst-Probe; der `/healthz`-Handler stempelt
`last_request_ms` aber **während** des Probes, also ein paar ms **später**. Damit
ist `last` (z. B. 100183) **größer** als `now` (100169), und `now_ms - last`
unterläuft auf `uint64_t` zu einer Riesenzahl → liest sich als „unendlich lange
stumm" (> 30 s Schwelle) → **Fehl-Restart alle ~45 s**, obwohl das Dashboard von
außen erreichbar war. Diagnose-Log: `tick=RESTART last=100183 now=100169
thr=30000 probe_err=0(ESP_OK) status=200`.
**Nicht im AP-aus-Scope; separat untersucht und behoben.**

## Fix „Sender 240 MHz, 0 Steps" (`feat/sender-associate-dfs`, 2026-10-17)
**Ergebnis: Auf dem ESP32 classic (IDF v6.1) sind ein zuverlässiger ESP-NOW-Link
und Funk-Powersave/DFS nicht gleichzeitig zu haben.** Am Gerät verifiziert.
- Ursache gefunden: `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` stand in
  `uart_sleep_init_idf()` **vor** `esp_wifi_init()` → Rückgabe
  `ESP_ERR_WIFI_NOT_INIT`, still ignoriert. Ohne Modem-Sleep hält der Treiber
  `ESP_PM_APB_FREQ_MAX` dauerhaft → DFS klebt bei 240 MHz.
- Der ESP-NOW-Wake-Window-Aufruf wurde von der IDF akzeptiert (`err=0`), half
  aber allein nicht.
- Mit **beidem** korrekt aktiv (`ESPNOW_SENDER_ESPNOW_POWER_SAVE=y`):
  Funk zykelt, DFS könnte skalieren — **aber der ESP-NOW-Link bricht**
  (Link-Watchdog: `sleep_fallback:1`, Light-Sleep aus).
- [x] Beide Aufrufe korrekt verdrahtet: `esp_wifi_set_ps()` **nach**
      `esp_wifi_start()`, Wake-Window (`esp_now_set_wake_window()` vor
      `esp_wifi_connectionless_module_set_wake_interval()`) ab `esp_now_init()`
      und bei jeder Assoziation. Ergebnis-Code landet im Diag-Event-Ring.
- [x] `CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y` in `sdkconfig.defaults`
      (nötig, damit das Wake-Window auch ohne Assoziation wirkt).
- [x] **Default: Funk bleibt an** (`ESPNOW_SENDER_ESPNOW_POWER_SAVE=n`,
      Assoziation `ESPNOW_SENDER_ASSOCIATE=n`). „Keine Daten verloren" hat
      Vorrang.
- [x] **Sender-DFS praktisch aus: Obergrenze 80 MHz, Untergrenze 40 MHz.**
      `CONFIG_ESPNOW_CPU_MAX_MHZ` hat jetzt `default 80 if ESPNOW_ROLE_SENDER`
      (Empfänger bleibt 240). Da der Funk im Default dauerhaft an ist, hält der
      Treiber `ESP_PM_APB_FREQ_MAX` und DFS kann ohnehin nicht unter die
      Obergrenze — die Obergrenze selbst auf 80 zu legen ist also das
      wirksame „DFS aus, 80 MHz laufen". Die 40-MHz-Untergrenze bleibt: wird
      der APB-Lock je freigegeben (Modem-Sleep-Experiment), darf der Takt auf
      40 fallen. CI prüft das pro Rolle (Sender 80, Empfänger 240).
- [x] Relay-OTA validiert das Sender-Image vor der Aktivierung (kein USB → sonst
      nicht wiederherstellbar).
- Hinweis: Mit `test sleep pin 80` läuft der Takt manuell auf 80 MHz; mit
      `test sleep restore` zurück auf die Obergrenze. `test cpu` zeigt jetzt
      „40-80 MHz … 0 steps" (Takt bleibt oben, weil der Funk den APB-Lock hält).

Stand: Branch `cline/power-history-diag` (Basis: On-Demand-Diagnose + Leistungshistorie).
Letzter Commit: `f5239c7` – CI **grün** (Push-Run `38466461726`, PR-Run `38466468521`:
Host-Tests, Sender, Receiver). Neu: JSON-Escaping der Sender-Diagnose-
Event-Texte (`f5239c7`, behebt `JSON.parse: bad control character`), CSV-Analyzer
`tools/analyze_power_csv.py` (`327ae24`). Davor: Sender-UART-Gap-Attribution + CPU-Takt-
Historie (`8f0e0ef`), Receiver-Build-Fix `g_last_sleep_exit_us` (`26d9865`), Transport-Gap-
Klassifizierung Verlust vs. Reorder (`8db666c`), `Cache-Control: no-store` fuer alle
HTML-Seiten (`c074b61`).
Flashbare Artefakte (nicht abgelaufen) im PR-Run `38466468521`: `sender-sml-flash-files`,
`receiver-sml-flash-files` (+ je `*-firmware`). Der Sender ist **ein** Image mit
adaptivem Light-Sleep als Default (`CONFIG_ESPNOW_SENDER_LIGHT_SLEEP=y`).
**Wichtig:** Der zuletzt geflashte Build `#156` (`82718b6`) enthält den
Mid-Telegram-Sleep-Fix **nicht** – für den Bench-Test den aktuellen Sender-Build
flashen (ab `f16cdc8`).

## ⏰ Arbeitsweise: Chat-Heartbeat (von jedem Folge-Chat zu übernehmen!)
Diese Session läuft in einer Sandbox mit **Inaktivitäts-Timeout (~20 Min)**. Wird der Chat
nicht benutzt, wird die Sandbox zurückgesetzt und nicht committete Arbeit ist weg (siehe
„Geborgene Arbeit" unten).

**Gemessen (2026-10-14, Memory-Sync `989f21c`):** Der `[heartbeat]`-Ping ist selbst ein
**User-Message-Post**. Obwohl er mit `run:false` kommt, definiert die Agent-Server-API `run`
als „agent loop automatisch starten **falls nicht laufend**" — ein **idler** Chat wird also
trotzdem geweckt: **77/104** Heartbeats kippten `idle -> running`, **27/104** lösten eine
echte Agent-Aktion aus (voller LLM-Turn, ~60k–200k Prompt-Tokens). Zusätzlich blockieren
lange `sleep N`-Polls im Terminal den Agent-Loop (gemessene Heartbeat→Aktion-Latenz bis
**1505 s**), echte User-Nachrichten warten genauso lange. **Der Ping macht den Chat träge.**

- **Neue Regel (verbindlich):**
  1. Die Sandbox **nur** mit `POST /api/v1/sandboxes/{id}/resume` warm halten — **keinen**
     `[heartbeat]`-Post mehr, der einen Run startet. Ein sichtbares Lebenszeichen darf
     keinen Agent-Turn auslösen.
  2. Nicht mit langem `sleep N` im Chat pollen — kurze Polls, damit der Chat für echte
     Nachrichten antwortet.
  3. Bei sehr langen Chats aggressiver verdichten (oder forken/neu starten); ein
     ~60k–200k-Token-Prompt macht jeden Turn langsam, unabhängig vom Heartbeat.
- **Was Folge-Chats tun sollen:**
  1. Beim ersten Schritt prüfen, dass die Keep-alive-Automation noch ACTIVE ist (Liste der
     Automations). Falls nicht: neu anlegen — aber als **resume-only** (`POST …/resume`),
     ohne Message-Post, damit kein Turn gestartet wird.
  2. Nach jedem bedeutenden Schritt **sofort committen + pushen** (kleine Commits) — der
     Heartbeat schützt nur den Chat, nicht die Arbeit. Nichts Wichtiges in `/tmp` halten.
  3. Ein eingehender `[heartbeat] … keine Aktion noetig`-Ping braucht **keine Aktion**; nur
     offene Arbeit aus diesem TODO weiterführen, falls vorhanden.
- Ein einzelner transienter `FAILED`-Lauf („Failed to get execution context") ist Infra,
  nicht der Script-Fehler — nur bei mehreren Fehlschlägen eingreifen.

## Offen

### ⚠️ Verbindliche Regel: SPIFFS-Historie muss erhalten bleiben (nicht verhandelbar)
Die persistente Leistungshistorie liegt in der SPIFFS-Partition `storage`
(Offset **0x320000**, Größe **0xE0000** = 917504 B, Datei `/powerlog/quarters.bin`,
bis zu 29000 Minutenwerte, ~20 Tage).

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
      ready: N of 29000` gegen `record_count` in `/api/power-history` abgleichen.
- [ ] Header-Handling prüfen: Nach dem Fix `1612975` (gültiger Fresh-Header + In-Place-
      Reparatur) darf ein gültiger Bestand **nicht** als „leer" gewertet werden.
- [ ] Migration prüfen (`4af921a`): Alte `format_version` darf die Datensätze nicht
      mehr verwerfen — nach dem Boot müssen migrierte Datensätze `record_count > 0` ergeben.
- [ ] Akzeptanzkriterium (Pflicht): nach Reboot sofort `record_count > 0`, kein RAM-Fallback,
      0 Schreibfehler. Am Gerät verifizieren.

### Push / Zugang (gelöst)
- [x] **Schreibzugang gefunden (2026-10-12).** `GITHUB_TOKEN` = GitHub-App-Token (`ghu_…`),
      read-only (`git push` -> `403`). `GITHUB_WRITE_TOKEN` = gültiges fine-grained PAT
      (`github_pat_…`, läuft 2026-11-11 ab), Prefix `github_pat_…` -> **darf pushen**
      (`git push --dry-run` bestätigt). Frühere `401 Bad credentials`-Notiz war ein
      alter/abgelaufener Tokenstand.
- [x] Beide Commits (`e7e7cb7` Tagesgrenzen, `af88bbd` Kapazitäts-Fix) auf
      `cline/power-history-diag` gepusht; CI grün (Run `38312096355`).
- Workaround: `origin`-URL trägt jetzt den Write-Token, `git push` funktioniert direkt.
      `gh` liest weiterhin `GITHUB_TOKEN` (Env schlägt gespeicherte Credentials) —
      dafür `GH_TOKEN=$GITHUB_WRITE_TOKEN gh …` setzen.
- [x] Bergungs-Branch `recovered/ams-features-salvage` (Commit `d10ea8c`) gepusht
      (`origin/recovered/ams-features-salvage`). Integration nach `main.c` bleibt offen.

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
      (v1-Datensatz wird verworfen), mit Fix grün - rot/grün verifiziert. **Gepusht** (in
      `origin/cline/power-history-diag` enthalten).
- [ ] **Health-Reboot / Selbstheilung**: Reboot, wenn z. B. `last_rx_ago_ms` > N Minuten oder
      Wi-Fi down > N Minuten. Nutzt vorhandenes `last_rx_ago_ms`. (Im geretteten
      `health_*`-Modul angelegt, aber nicht integriert.)
- [x] **Rollback-Pfad verdrahtet** (`boot_confirm_poll`): `BOOT_TAKE_ROLLBACK` wurde
      berechnet, aber **nie ausgeführt** — nur `BOOT_TAKE_CONFIRM` wurde behandelt, und
      `boot_confirm_fail()` hatte **null Aufrufer**. Ein Image, das zwar hochkommt, aber das
      Dashboard nicht innerhalb der 30 s Grace bereitstellt, blieb damit hängen. Jetzt ruft
      `boot_confirm_poll()` bei `BOOT_TAKE_ROLLBACK` `boot_confirm_fail("dashboard did not come up")`
      auf → `esp_ota_mark_app_invalid_rollback_and_reboot()`. Die Abbildung
      Aktion→Wirkung liegt jetzt rein in `boot_policy.c` (`boot_action_effect`).
      Fehlerfälle doppelt host-getestet: `test_boot_policy.c` (Entscheidung, 53 Checks,
      tabellengetrieben: nie oben / früh gestorben / spät gestorben / Flattern / zu spät oben
      → Rollback; gesund + exakt zur Grace → Confirm) und `test_boot_confirm_trigger.c`
      (echter `boot_confirm.c`-Kleber löst für jeden Fall genau **einen** Rollback aus;
      bestätigtes Image rebootet nur, rollt nie zurück — 39 Checks).
- [x] **CSV-Restore + Sofort-Refresh (Dashboard):** Buttons „Datenbank jetzt aktualisieren"
      (`POST /api/db-refresh` → `history_backfill_request_full()`) und „CSV hochladen"
      (`POST /api/power-history-upload`). Der Upload streamt zeilenweise (ein volles Backup
      ~29000 Zeilen passt nicht in den Heap), parst mit `main/history_csv_import.c` (rein,
      host-getestet) und hängt in 64er-Batches sortiert über
      `power_history_import_sorted()` an. Dedupe pro Minute, **vorhandene Daten werden nie
      überschrieben**, kein Evict im vollen Ring. Tests: `test/test_history_csv_import.c`
      (28 Checks) + `test/test_history_csv_import_store.c` (23 Checks, echter Store).
- [x] **TWDT explizit** (erledigt): Die langlaufenden Worker sind beim Task-Watchdog
      angemeldet. Neues, kleines Modul `main/task_wdt_policy.{h,c}` (rein, host-getestet)
      hält die Policy (welche Worker je Rolle, wie lange sie legitim blockieren dürfen,
      welche Timeout-Math das abdeckt); `main/task_wdt_guard.{h,c}` ist der IDF-Kleber
      (`esp_task_wdt_init`/`_reconfigure` mit großzügigem Timeout + `trigger_panic`,
      `esp_task_wdt_add`/`_reset`). `power_history_worker` (beide Rollen) und die
      Sender-UART-Worker `uart_event_worker`/`uart_capture_worker` abonnieren sich selbst und
      füttern den Wächter pro Schleife; der 60-s-Batch-Wait des Writers ist jetzt begrenzt
      (nicht mehr `portMAX_DELAY`), damit ein idle (aber gesunder) Writer füttert und **nicht**
      als Stall gilt. Timeout 120 s (deckt 60 s-Batch mit 2x-Marge), Panic an → ein
      festhängender Worker rebootet statt still zu hängen. Der `status`-Konsolenbefehl zeigt
      „task watchdog: armed, timeout … , panic on, N worker(s)". Host-Test
      `test/test_task_wdt_policy.c` (115 Checks, im Runner). CI prüft beide Rollen-Images auf
      die Registrierungs-Strings.
- [x] **Post-OTA-Rollback-Bestätigung, Empfänger** (integriert): `boot_policy.c/.h`
      (reine, host-testbare Entscheidung) + `boot_confirm.c/.h` (Kleber) ersetzen das
      unbedingte `esp_ota_mark_app_valid_cancel_rollback()` im Empfänger. Bestätigung erst
      nach **45 s** stabil + Dashboard oben; kommt das Dashboard nach **30 s** Grace nicht
      hoch → Rollback in den vorigen OTA-Slot (der Rollback-Zweig ist jetzt verdrahtet,
      siehe oben). Sender bewusst unverändert (hat kein
      Dashboard) — eigener Confirm-Pfad für den Sender ist offen. Tests:
      `test/test_boot_policy.c` (10 Checks, rot/grün verifiziert), im Runner aktiv.
      **⚠️ Hardware-Verifikation offen:** der echte OTA→bestätigt / OTA→Rollback-Pfad ist
      am Gerät zu prüfen (host-seitig nur die Entscheidungslogik abgedeckt).
- [x] **SML-Golden-Fixtures**: `test/fixtures/sml/ehz_bin.upstream.h` (echtes EMH-eHZ-
      Telegramm, 319 B, aus dem öffentlichen libsml-testing-Korpus) + Decode-Golden-Test
      `test/test_sml_parser_golden.cpp` (8 Checks; 1-0:1.8.0/1.8.1 Wh, 1-0:15.7.0 W exakt;
      gekipptes Byte → `SML_CHECKSUM_ERROR`). Im Host-Runner aktiv, ASan+UBSan.
- [ ] **AP abschalten, wenn STA verbunden** (größter einzelner Spareffekt): Empfänger fährt
      dauerhaft `WIFI_MODE_APSTA` (`main.c`); nach `GOT_IP` auf `WIFI_MODE_STA` wechseln.
- [ ] **Tages-/Monats-Summen im Flash** (`hImport[24]`/`dImport[31]` wie AMS): aus
      Minutenwerten ableiten, platzsparend + schnellere Langzeitgrafik.
- [x] **Letzte bekannte Zeit persistieren** (`lastMeterReadTime`): nach Reboot ohne SNTP ist
      `bucket_start==0` → keine Aufzeichnung; letzte UTC im Header/NVS ablegen.
      **Erledigt** (PR #26): reine Module `main/wall_clock.{c,h}` (Record-Format +
      CRC32, Seed-/Bucket-Mathematik, Persist-Policy, `wall_clock_utc_is_sane()`)
      und `main/wall_clock_store.{c,h}` (NVS `wallclock`/`last`, weist unsinnige
      Zeiten ab). Beim Boot wird die Uhr vor dem ersten SNTP-/ESP-NOW-Sync aus dem
      gespeicherten Wert gesetzt (übersprungen, wenn schon gültig), als *untrusted*
      markiert; sofortiger Persist bei echtem Sync, sonst alle 300 s. Nur ein echter
      Sync setzt den Boot-Marker (Sender-Offline-Lücke). On-Disk-Format unverändert,
      `storage`-Partition unangetastet. Host-Test `test/test_wall_clock.c` (40 Checks,
      Fork-„Reboot" über NVS-Shim: Uhr läuft weiter, `bucket_start != 0`).
- [x] **MQTT-Availability (LWT)**: `main/mqtt_bridge.c` setzt ein LWT auf
      `<prefix>/status` (retained, offline) und veroeffentlicht beim Connect `online`;
      der Discovery-Hub verweist per `availability_topic` darauf.
- [ ] **MQTT-Service-Events (Reboot-Grund)** — ergänzt Crash-Journal (noch offen).
- [ ] Optional: konfigurierbares Reboot-Intervall (nächtlich), Deckelung wie AMS.

### SPIFFS-Historie auch im Sender (gleiche Speicherung wie Empfänger)
Ziel: Der **Sender** soll ebenfalls die persistente Minutenhistorie bekommen und die Daten
**genau wie der Empfänger** ablegen (gleiches `power_history`-Format, gleiche Datei).
- Ausgangslage (Stand 2026-10-12 geprüft): Die lokale Sender-Historie läuft **bereits** —
  `power_history_init()`, `g_power_sample_timer` und der History-Worker stehen in `app_main`
  **außerhalb** der Rollen-`#if` (gemeinsamer Pfad) und der Sender speist sie über
  `record_sender_power()` aus den UART-Werten. Partition `storage` (`0x320000`/`0xE0000`)
  ist in `partitions.csv` für beide Rollen identisch → keine Repartitionierung.
- Zu tun / offen:
  - [x] Aufzeichnung + Persistenz im Sender aktiv (gemeinsamer Init-Pfad).
  - [ ] **Verifikation am Gerät:** `record_count > 0` beim Sender nach Flash (kein RAM-Fallback).
  - [ ] **Sender-Zeitbasis:** ohne Empfänger keine Wall-Clock → `bucket_start==0` sauber
        behandeln (hängt an „letzte Zeit persistieren", s. o.); Wall-Clock-Sync (`a8a3e23`)
        muss vor dem Stempeln gelaufen sein.
  - [ ] Host-Test + CI: Persistenz-Test (`test/test_reboot_persistence.c`) auch für den Sender.
- Akzeptanz: Sender legt die Minutenhistorie im Flash ab und stellt sie nach Reboot wieder her
  (`record_count > 0`, kein RAM-Fallback) — identisch zum Empfänger.

### Sender: gepufferte ESP-NOW-Übertragung im 5-s-Zyklus
Ziel: Der Sender sammelt seine aus der UART dekodierten Werte **lokal** und schickt sie
**gebündelt** (statt ein Paket pro Telegramm). Statt dauerhaft zu funken, arbeitet er in
einem Zyklus: sammeln → ~5 s → Pakete senden → (Radio nutzbar/ruhen).
- [x] Puffer-Modul `main/tx_batch.{c,h}`: `sml_tx_item_t` + Ringpuffer
      (`sml_tx_batch_push/take/count/dropped`), host-getestet
      (`test/test_tx_batch.c`, im Runner). Verdrängt bei Voll das **älteste** Element
      (monoton wachsende Sequenz bleibt erhalten, kein Sprung an der Spitze).
- [x] Sequenz-/Kantenlogik für den Abgleich: `main/tx_batch.c`
      (`sml_tx_batch_mark_sent`, `sml_tx_batch_missing`) — reine Funktionen, host-getestet.
- [x] Sendeschleife in `uart_transport_worker` umgebaut: `sml_adapter_telegram_callback()`
      pusht in den Ring (`sml_tx_batch_push`), `uart_transport_worker` wartet
      `SML_TX_CYCLE_MS` (5000) und ruft dann `flush_tx_batch()` — bis zu `SML_TX_BURST`
      (32) Telepgramme älteste-zuerst, Semaphore je Paket. Kein Paket mehr pro Telegramm.
      Sequenz kommt aus dem Ring; `sml_tx_batch_mark_sent()` nur bei **bestätigter**
      Zustellung (unbestätigt bleibt „missing" → Abgleich).
- [x] **Kein Verlust bei fehlgeschlagener Zustellung**: `send_sml_telegram()` gibt jetzt
      `bool` zurück (nur `ESP_NOW_SEND_SUCCESS` = true); bei Fehlschlag legt
      `flush_tx_batch()` das Element mit `sml_tx_batch_requeue_front()` an den Kopf
      zurück und beendet die Phase → nächster Zyklus sendet es erneut. Vorher war ein
      unbestätigtes Telegramm aus dem Ring verschwunden und wurde nie wiederholt.
      Host-Test erweitert (`test_tx_batch.c`, 55 Checks: Requeue an den Kopf,
      Reihenfolge, Voll-Ring-Verdrängung beim Requeue).
      Einschränkung: Telemetrie wird per **Broadcast** gesendet, der Callback meldet
      damit die Treiber-Übergabe, kein echtes MAC-ACK — der Retry greift nur bei echten
      Treiberfehlern. Echtes Delivery-Confirm bräuchte Unicast (wie die Diagnose).
- [x] Optionale Funkruhe zwischen Zyklen: jetzt **umgesetzt** über
      `CONFIG_PM_ENABLE` + DFS + Tickless-Idle (variabler Takt 80↔40 MHz, Light-Sleep
      im Idle). Die frühere Sorge (ESP-NOW-Verluste) ist durch den Backfill abgedeckt
      (s. u.): verlorene Funkminuten holt der Empfänger aus der Sender-SPIFFS nach.
      Details: `sdkconfig.defaults`, CI-Job „Verify the variable CPU clock".
- [x] Zähler/Diagnose: `batch_pending` + `batch_missing` im `espnow_diag_counters_t`
      ergänzt; im `/api/sender-diag`-Counters-JSON mit ausgegeben. `tx_queue_drops`
      zählt jetzt Ring-Verdrängungen (Voll).
- [x] Host-Test `test/test_tx_batch.c` (55 Checks): Bündelung, Verdrängung, kein Verlust
      innerhalb eines Zyklus, `missing`-Arithmetik, Requeue/Retry. In `run_host_tests.sh`
      + CI.
- **Am Gerät zu prüfen:** Sender-Bootlog „sent every 5000 ms (burst 32)"; Empfänger
      bekommt weiterhin alle Minutenwerte; `batch_missing` bleibt 0. Zusätzlich: unter
      aktivem PM/DFS/Light-Sleep prüfen, dass ESP-NOW-Empfang stabil bleibt (sonst
      Backfill als Netz, oder `CONFIG_PM_ENABLE` wieder entfernen).
- Akzeptanz: Der Sender funkt zwischen den Zyklen nicht mehr pro Telegramm einzeln,
  verliert dabei keine Minutenwerte (Puffer groß genug für einen Zyklus) und beide
  Rollen bauen in CI.

### Sender: adaptiver Light-Sleep zwischen Telegramm-Bursts (NEU, `0b9f637`)
Ziel: „Mit 400 ms bauen und der Sender passt die Zeit automatisch an" — Sparsamkeit mit
variablem Takt, ohne dass ein Telegramm verloren geht. Feature ist **Default AN**
(`CONFIG_ESPNOW_SENDER_LIGHT_SLEEP=y` in `main/Kconfig.projbuild`) — der Sender ist **ein**
Image, kein separates `sender-sleep`-Artefakt mehr.
- [x] Regler `main/uart_sleep_window.{h,c}`: Start **400 ms**; Obergrenze ist immer
      `kürzeste beobachtete Zwischen-Telegramm-Lücke × 3/4`; **+25 ms** pro sauberem Schlaf;
      **halbiert** sich, wenn ein Telegramm im Fenster ankam. `ESP_PM_NO_LIGHT_SLEEP`-Lock
      hält die CPU für den ganzen Burst wach (sonst würde die APB-Clock die UART-FIFO
      aushungern); `esp_pm_configure()` (DFS 80↔40 MHz + Light Sleep) + `WIFI_PS_MIN_MODEM`.
- [x] **Wachbleiben bei Anforderung/OTA:** `uart_sleep_hold()` — der ESP-NOW-Empfangspfad
      erneuert einen selbstlöschenden Hold (**5 s OTA, 2 s History-/Diag-Request**). Kein
      Acquire/Release-Paar über viele Pfade ⇒ kein Leck.
- [x] Tests: `test/test_uart_sleep_window.c` (21 Checks) + `test/test_uart_sleep_sim.c`
      (14 Checks, simuliert den APB-Stop: ein Telegramm im Schlaf verliert Bytes).
      Ergebnis: pendelt ein (1 s ⇒ ~616 ms, 2,5 s ⇒ ~1600 ms), ein plötzlicher
      Schneller-Wechsel kostet genau **1** Telegramm, dann adaptiert er.
- [x] Diagnose: `sleep_window_ms`, `sleep_min_gap_ms`, `sleep_hold_active` in
      `/api/sender-diag`-Counters + Dashboard-Text (Counters 160 B < 250 B ESP-NOW-Limit).
- [x] CI: Syntax-Check des **aktivierten** Zweigs (`gcc -fsyntax-only` mit IDF-Stubs in
      `test/host/include`), damit der sonst ausgeblendete Pfad nicht verrottet.
- [ ] **Am Gerät verifizieren (offen):** Nach dem Flashen des *neuen* Sender-Images muss
      `adaptive sleep window: 400 ms` erscheinen (nicht `0 ms`), `light sleep enabled` +
      `uptime_sleep_ms > 0`; `parser_resyncs`/`gap_loss` bleiben **0**; Receiver bekommt
      weiter alle Minutenwerte. Wenn nicht: Backfill als Netz nutzen, oder Feature aus lassen.
- [x] **🐛 Wurzel gefunden: `0 ms` trotz `build profile: sender-sleep` (BEHOBEN).**
      Die Guard `#if CONFIG_ESPNOW_SENDER_LIGHT_SLEEP && SOC_PM_SUPPORT_LP_TIMER &&
      (SOC_PM_SUPPORT_EXT_WAKEUP || SOC_PM_SUPPORT_GPIO_WAKEUP)` war auf dem ESP32 **immer
      falsch**: `SOC_PM_SUPPORT_LP_TIMER` existiert im `soc_caps.h` des ESP32 **nicht**
      (`grep -c` = 0; es gibt nur `SOC_LIGHT_SLEEP_SUPPORTED`). Damit wurde trotz gesetztem
      Config der **Stub** kompiliert → `uart_sleep_init_idf()` = `false`,
      `uart_sleep_window_ms()` = `0`, kein `esp_pm_configure()`. Symptom am Gerät:
      `build profile: sender-sleep` **und** gleichzeitig `window: 0 ms` /
      `light sleep disabled` / `slept: 0.0 s`.
      Fix: Guard auf `SOC_LIGHT_SLEEP_SUPPORTED` reduziert (`main/uart_sleep_window.c`);
      Host-Stub `test/host/include/soc/soc_caps.h` und CI-Syntax-Check entsprechend
      umgestellt. **CI-Grep entwaffnet:** Er prüfte `main.c`-Text, der auch im Stub-Build
      vorkommt; jetzt greppt er `adaptive sleep path compiled in` aus dem **echten** Zweig
      von `uart_sleep_window.c` (im Stub nicht enthalten) → ein Sender-Image ohne Sleep-Code
      fällt jetzt auf.
- [ ] Optional: `sleep_min_gap_ms` über die Diag-Seite im Dashboard als eigene Zeile ausgeben
      (bisher nur im Text).
- [x] **Ein Sender-Image mit Sleep als Default:** `CONFIG_ESPNOW_SENDER_LIGHT_SLEEP=y`
      ist Default in `main/Kconfig.projbuild`; die CI-Matrix hat nur noch `sender` und
      `receiver` (das separate `sender-sleep`-Artefakt ist entfallen). Ein CI-Schritt prüft
      per `grep` in `sdkconfig` **und** im gebauten `esp32-IR.bin`, dass der Sleep-Pfad
      wirklich enthalten ist. `build_profile` im `counters`-Diag zeigt zur Laufzeit, welches
      Image läuft (1 = mit Sleep). (`.github/workflows/esp-idf-build.yml`)

### 🐛 Sender bleibt „HELD AWAKE", `slept: 0.0 s` (BEHOBEN 2026-10-15)
Symptom (am Gerät, T+38 s nach Boot): `adaptive sleep window: 100 ms (shortest gap
seen: 691 ms)` und gleichzeitig `sleep: awake … slept this session: 0.0 s · HELD AWAKE
(data / OTA request active)`, CPU bei `80 MHz now`. Die Kalibrierung hatte also eine
691-ms-Lücke gesehen (Obergrenze 518 ms), der Schlaf lief trotzdem nie.
Ursache: **Jeder** ESP-NOW-Diag-Request setzte in `receive_callback()` einen 2-s-Hold —
auch ein reiner Counters-/Events-Statistik-Poll. Das Dashboard/der Receiver fragt die
Counters aber im Sekundentakt ab, damit wurde der Hold ständig erneuert und lief nie ab
(`uart_sleep_hold_active()` blockiert dann `uart_sleep_maybe_sleep_idf()`). Zweitens zählte
der Regler ein Fenster als „sauberen Schlaf", sobald nach dem `vTaskDelay` nichts im
UART-Puffer lag — auch wenn der `vTaskDelay` wegen des Holds gar nicht geschlafen hat.
- [x] `uart_sleep_hold(2000)` nur noch bei **echten Datenflüssen**: `ESPNOW_DIAG_SECTION_HISTORY`
      oder `_CMD` (OTA hält weiterhin 5 s). Ein Counters/Events-Poll hält die CPU nicht mehr.
- [x] Sauberer Schlaf nur bei **echtem** Schlaf: `sleep_exit_callback()` speist die real
      akkumulierte Light-Sleep-Zeit via `uart_sleep_set_slept_ms()` ein; `uart_sleep_note_clean()`
      wächst nur, wenn `slept_ms != 0`. Sonst wächst der Regler ein Fenster, das er nie getestet hat.
- [x] Host-Test: `test_no_sleep_does_not_grow_the_window` (Fenster bleibt bei `slept_ms == 0`).
      `test_uart_sleep_window.c` 21 → **25 Checks**, alle grün.
- [ ] Am Gerät verifizieren: nach dem Flashen müssen `slept this session > 0` und (ohne
      History-Pull) `HELD AWAKE` verschwinden; `parser_resyncs`/`gap_loss` bleiben 0.

### Versionierung + Commit-Konvention
- [x] `VERSIONING.md`: SemVer-Tag `vMAJOR.MINOR.PATCH` ist die Wahrheit (erste Baseline
      `v0.1.0`); `git describe` schreibt sie in `main/build_version.h` → `fw_version()`.
      MAJOR steigt bei Wire-/Speicher-Bruch, MINOR bei neuer Funktion, PATCH bei Bugfix.
- [x] Sender-Version über den Draht: `espnow_diag_counters_t.fw_version` (aus `fw_version()`),
      `ESPNOW_DIAG_VERSION` 1 → 2; Receiver zeigt sie auf der Senderseite als `sender fw:`.
      Frame bleibt in einem ESP-NOW-Payload (16 + 204 = 220 ≤ 250, `_Static_assert`).
- [x] Commit-Konvention: je Commit nur die Dateien der weiterentwickelten Seite; `feat(both):`
      wenn beide (z. B. Wire-Format). `main/main.c` trägt beide Rollen → Diff hunk-genau teilen.
- [x] **Regression behoben (2026-10-15):** Der Sprung auf `ESPNOW_DIAG_VERSION 2` ließ den
      Sender **jede** Anfrage eines nicht neu geflashten Receivers verwerfen (`is_diag_request`
      verlangte exakt `== 2`) → der Receiver sah den Sender als tot. Jetzt akzeptiert
      `is_diag_request`/`is_diag_response` `ESPNOW_DIAG_VERSION_MIN..ESPNOW_DIAG_VERSION`, und
      **jede** Antwort spiegelt die Version der Anfrage zurück (counters/events/history/crash/
      cmd/time-sync), damit auch ein alter Receiver die Antworten annimmt. Header-Layout
      unverändert, Counters-Payload nur am Ende gewachsen → kompatibel.

### 🐛 Sender-Diagnose: „DID NOT END CLEANLY" bei bewusstem Reboot (BEHOBEN)
Symptom: Nach OTA/Console-Reboot meldete der Sender beim nächsten Boot
„Previous session: DID NOT END CLEANLY" mit `reset reason: software-restart (3)` —
obwohl kein Absturz vorlag.
Ursache: `clean_shutdown` wird nur gesetzt, wenn der registrierte Shutdown-Handler läuft.
Die bewussten Reboots rufen `esp_restart()` direkt auf (Sender: OTA-Ende `main.c:759`),
da laufen keine Shutdown-Handler mehr → Journal bleibt „nicht clean".
- [x] `crash_journal_reset_is_deliberate(reason)` (reine Funktion, host-getestet):
      `software-restart (3)` und `deep-sleep (8)` = gewollt; `panic/watchdog/...` = echter Fehler.
- [x] Vor den bewussten `esp_restart()`s wird jetzt `crash_journal_mark_clean_restart()`
      aufgerufen (Sender-OTA-Pfad; Receiver-Pfade rufen es ebenfalls auf).
- [x] Bootlog + Dashboard zeigen bei gewolltem Reboot „restarted on purpose (not a crash)"
      statt „DID NOT END CLEANLY" (neues Diag-Feld `deliberate_restart`).
- [x] Test: `test/test_crash_journal.c` 19 → **24 Checks** (deliberate-Klassifizierung).

### 🐛 Empfänger-Dashboard: im Live-Modus nur 15-Min-Balken (BEHOBEN)
Symptom: In **jedem** Live-Zeitraum (2/6/12/24 h/3 T/7 T/Max) zeichnete das Dashboard nur
die 15-Minuten-Tagesbalken; der Slider schien wirkungslos.
Ursache: Ein **Einheiten-Mismatch**. `updateHistory()` setzte
`windowStart=Math.floor(endMs/1000)-hours*3600` (Sekunden), aber die Record-Timestamps
sind **Millisekunden** (`bucket_start_ms = window_start_utc*1000 + …`). Im `drawHistory`-
Wächter `t<start||t>end` liegt `start` damit ~1000× zu klein → **jeder** Balken wird
verworfen; nur der Tagespfad (`windowStart=Date.parse(day+'T00:00:00Z')`, ms) rendert.
- [x] Fix in `main/main.c` (`sml_page`): `windowStart=liveWindowStartMs(hours,endMs)` (ms);
      der `?start=`-Query bleibt Sekunden (`toUtcSeconds(windowStart)`), ebenso der
      Vergleichs-Start (`toUtcSeconds(windowStart)-86400`).
- [x] Test: `test/check_dashboard_js.py` „live window units" — ms-Start zeigt alle Balken,
      Sekunden-Start clippt alle (rot/grün-Gegenprobe).
- [ ] Verifizieren am Gerät: Live-Slider 2 ↔ 24 h → Auflösung wechselt, Balken bleiben sichtbar.

### 🔎 Sender-Diag: Ring-Flooding durch „diag response confirmed" (BEHOBEN)
Symptom: Im Reiter „Recent events" tauchten fast nur `diag response confirmed flags=…`
auf, die eigentlich interessanten `rx gap`-Zeilen wurden verdrängt; man sah scheinbar
„weniger" Diagnosepunkte.
Ursache: **Kein Logikfehler, sondern Beobachtungs-/Ring-Effekt.** Der Event-Ring fasst nur
`ESPNOW_DIAG_EVENT_RING = 16` Einträge. Jedes Antwortpaket schrieb eine Zeile
`diag response confirmed`; ein events-Fetch über mehrere Seiten (4 Events/Paket) erzeugt
**ein Paket pro Seite**, also viele solcher Zeilen. Der Ring lief voll → die UART-/Gap-Events
wurden herausgeschoben. (Ein Auto-Refresh-Zyklus ruft die Diag alle 5 s ab und schreibt
zusätzlich weitere Zeilen.)
- [x] `diag response confirmed` wird nur noch **einmal pro Anfrage** (Seite 0) geloggt;
      Fehlerfälle (`not confirmed`, Timeout) bleiben vollständig. Damit bleibt der Ring
      für echte Ereignisse frei. (`main/main.c`, `espnow_diag_worker`.)

### ⚠️ Sender-Sleep: CPU schlief MITTEN im Telegramm (BEHOBEN)
Symptom: Mit dem Sleep-Sender-Build (früher separat als **sender-sleep** geflasht)
häufige `rx gap … mid-telegram` (~50–90 ms, ~1/s) und die Sorge, ob `espnow sleep`
sich falsch verhält.
Ursache (echter Bug, kein Messartefakt): Der UART-Read-Timeout ist **50 ms**, das
SML-Zählwerk pausiert aber **innerhalb** eines Telegramms ebenfalls ~50–90 ms. Der
Idle-Zweig feuerte also mitten im Telegramm, löschte „Burst aktiv" und schlief
~100 ms → der Telegramm-Rest wurde abgeschnitten (Checksum-Fehler / Datenverlust).
Zusätzlich wurde das Schlaffenster aus genau diesen ~80 ms Pausen berechnet
(auf `min_ms = 100 ms` gepinnt) und `uart_sleep_note_short_sleep()` (Backoff bei
zu langem Schlaf) war in der Firmware **nie verdrahtet**.
- [x] Nur schlafen bzw. das Fenster aktualisieren, wenn `sml_adapter_awaiting_telegram()`
      „zwischen Telegrammen" meldet (`main.c`).
- [x] `uart_sleep_note_byte(..., awaiting)`: nur echte Telegramm-Pausen verkleinern
      das Fenster (mid-telegram-Gaps werden ignoriert).
- [x] Nach dem Fenster RX-Puffer prüfen: leer ⇒ wachsen (`note_clean`), Bytes vorhanden
      ⇒ Backoff (`note_short_sleep`) — jetzt tatsächlich aufgerufen.
- [x] Host-Test `test_mid_telegram_gap_does_not_shrink_the_window` (24 Checks) +
      Stub `driver/uart.h` für den Sleep-Syntax-Check.

### ℹ️ Sender: viele `mid-telegram`-Gaps — Einordnung
Bewertung: `mid-telegram` heißt, die Stille liegt **innerhalb** eines laufenden
Telegramms (`!sml_adapter_awaiting_telegram()`). Bei **aus** geschaltetem Sleep sind
das normale Pausen des IR-Lesekopfs (kein Verlust: `gap_loss` steigt nur bei
CRC-Fehler, `main.c:1963`; bei `frame/parity err 0` und `sml ok/fail` ohne Fehler ist
alles gut). Bei **an** geschaltetem Sleep waren sie bis zum Fix oben ein echtes
Problem. **Faustregel:** `gap_loss == 0` und `frame/parity err == 0` ⇒ normal;
erst `gap_loss > 0` oder steigende `parser_resyncs` sind echt.

### ✅ Gap-Ursachen im Gerät selbst zugeordnet + alle CPU/Sleep-Daten sichtbar
Damit die Frage „woher kommt der Gap?" nicht mehr geraten werden muss:
- [x] Gap-Log **ratenbegrenzt** (10 s) und mit **Ursache** gelabelt:
      `cause=meter` (Zähler-Pause), `sleep` (Schlaffenster), `stall` (lange Pause
      ohne Schlaf), `buffer` (UART-RX voll). Einmalige Startzeile erklärt, dass nur
      `gap_loss` Verlust ist.
- [x] Zuordnung im Capture-Worker: buffer voll > sleep (Gap ≥ Fenster **und** gerade
      aufgewacht) > meter (kurz/zwischen) > stall (lang mid-telegram, kein Sleep).
      Zähler `gap_cause_{meter,sleep,stall,buffer}` + `gap_last_cause`.
- [x] CPU-Takt-Historie (1-s-Sampling): Session min/avg/max MHz + Anzahl Taktschritte.
- [x] Crash-Journal v2: CPU-Takt-Historie + Gap-Ursachen; Event-Text 32 B, damit
      Header+Journal ≤ 250 B bleiben (243 B geprüft).
- [x] Diag-Seite: Präzedenz-Bug behoben (die Modem-Sleep-Zeile verwarf per
      `+ … ? … : …` den ganzen Rest der Ausgabe) und die neuen Zeilen ergänzt
      (CPU-Historie, Gap-Quellen, Klartext-Urteil **NO DATA LOST / DATA LOSS**).

**Wichtig zum Testen:** Der zuvor geflashte Build (`#156`, Commit `82718b6`) enthält
den Mid-Telegram-Sleep-Fix **nicht**. Für den Test den **aktuellen** Sender-Build
flashen (Commits ab `f16cdc8`), sonst sieht man weiter die alte Symptomatik.

### ✅ Empfänger: „Transportlücken" sauber klassifiziert (Verlust vs. Reorder)
Wichtig: Diese Gaps gibt es **auch ohne CPU-Sleep** — sie stammen **nicht** vom Sleep.
Die Sequenz wird im Sender **pro Paket** vergeben (`sml_tx_batch_push`), ein noch
nicht gesendetes Paket kann per `sml_tx_batch_requeue_front` mit seiner **alten**
Sequenz erneut vorne eingereiht werden (Retry). Daher:
- [x] **Rückwärts/Duplikat** (`sequence <= last`, Retry/Reorder) = **rückgewonnen**,
      **kein** Verlust, **kein** Reset der 60-s-Sammlung mehr.
- [x] **Vorwärtssprung** (`sequence > last+1`) = echtes über die Luft verlorenes
      Broadcast-Paket (kein ACK); nur dann wird die 60-s-Leistungssammlung
      zurückgesetzt. Fehlende Daten kommen weiterhin per Backfill nach.
- [x] Zähler getrennt: `transport_losses` (Verlust) vs. `transport_reorders`
      (rückgewonnen). `/api/sml` + `/api/dashboard` liefern beide; die Meter-Seite
      zeigt „Funkpakete verloren: N (rückgewonnen/Reorder: M)".
- [x] Wert-Ring von 64 → 256 vergrößert. Der Überlauf (`overflow=true`) ist ein
      **reines Anzeige-Artefakt** (Browser-Cursor älter als der RAM-Ring), **kein**
      Messverlust — die Meldung heißt jetzt entsprechend „DISPLAY GAP …" statt
      „VALUE GAP DETECTED".
- [x] 5-s-Zyklus (`SML_TX_CYCLE_MS 5000`, `SML_TX_BURST 32`) ist der Auslöser der
      Burst-Verluste (~0,65 %), aber **nicht** die Ursache der Zählung selbst.

### Synchronisation: Empfänger holt fehlende Sender-Daten (Backfill)
Ziel: Der Empfänger soll Minutenwerte, die er **nicht** mitbekommen hat (Funklücke,
Sender/Receiver-Neustart, OTA), **nachträglich vom Sender** holen — der Sender hat sie
in seiner eigenen SPIFFS-Historie (siehe oben).

**Zwei komplementäre Signale, nicht verwechseln:**
1. *Telegramm-Lücken (Sekunden, Funk):* der Sender meldet über
   `sml_tx_batch_missing` (Diag-Feld `batch_missing`), welche Sequenzen nie bestätigt
   gesendet wurden. Das ist der **Live-/Regel**-Abgleich für den Batch-Zyklus.
2. *Minuten-Historie-Lücken:* der Empfänger holt fehlende **Minutenrecords** paginiert
   aus der Sender-SPIFFS nach (unten). Das ist der **Backfill** nach Ausfall/Neustart.
   Die Telegramm-Sequenz ordnet dabei zu, welche Minuten betroffen waren.

- [x] Neuen Diagnose-Abschnitt `history` im vorhandenen ESP-NOW-Diag-Protokoll ergänzt:
      Request = `after_sequence`/`from_utc`/`to_utc`; Response = eine Seite Records + `more`
      (`espnow_diag_history_request_t` / `_response_t` in `main.c`, Flag `0x0010`).
      Platz: ESP-NOW-Payload 250 B, `power_history_record_t` = 28 B → 7 Records/Paket
      (der `_Static_assert` in `main.c` verhindert, dass Header+Trailer die 250 B sprengen).
- [x] Pagination mit `more`-Flag und fester Seitengröße (7 Records). Request/Response,
      Timeout/Fehlerzähler. (Kein separates ACK/Retry-Gremium — fehlgeschlagene Runden
      brechen die Session ab und werden beim nächsten 30-min-Lauf wiederholt.)
- [x] Empfänger fordert gezielt „alles danach“ an (Zeitfenster + Cursor) und **merged
      dedupliziert**: `main/history_backfill.{c,h}` (Präsenz-Bitmap pro Minute, host-getestet)
      + `power_history_merge()` (validiert/migriert, reiht ein; Store nummeriert um).
      Live-Daten des Empfängers bleiben erhalten, der Sender füllt nur Lücken.
- [x] Voraussetzung Zeit: Der Sender stempelt mit Wall-Clock (Sync `a8a3e23`); der
      Empfänger startet eine Session nur bei gültiger lokaler Uhr und fordert nur das
      Fenster `[now-3d-2min, now-2min]` an (`bucket_start==0`-Sätze liegen außerhalb).
- [x] Rate/Dauer: nur **inkrementell** (nur die fehlenden Minuten) und gedeckelt
      (Session max. 256 Seiten; Default-Fenster 3 Tage). Große Nachladungen bleiben bewusst
      außen vor (dauern Stunden); das Fenster deckt Funklücken/Neustarts ab.
- [x] Auslöser: automatisch ~15 s nach Boot und alle 30 min (`history_backfill_worker`),
      **und** manuell: `POST /api/history-pull` (X-OTA-Key), Status per `GET /api/history-pull`
      (`merged_total`/`pages_total`/`errors`), Konsolen-Befehl `pull` (Empfänger).
- [x] Host-Tests: `test/test_history_backfill.c` (27 Checks, Fenster/Dedupe),
      `test/test_history_backfill_merge.c` (29 Checks, echter Store: Merge + Reject korrupt
      + `power_history_read_window`-Paging). Im Runner + CI.

**Quelle der Wahrheit:** Sender = Quelle (er liest den Zähler direkt), der Empfänger merged
nur die Lücken und überschreibt nie eigene Werte — so umgesetzt. Die Minutenhistorie wird
auf **beiden** Rollen lokal geschrieben (SPIFFS), der Sender speichert also alles selbst.

### 🐛 Bug: doppelte Minuten in der History nach Backfill (NEU, 2026-10-14)
Symptom: `GET /api/power-history.csv` liefert **doppelte Minuten**. Empfänger-Export
14.10.: 4828 Zeilen / 3557 eindeutige Minuten → **1271 überzählige Zeilen**; ein früherer
Export 615 doppelte Minuten / 1047 Zeilen. Die Dubletten-Paare liegen im `sequence` weit
auseinander (= zwei Zähler: Empfänger-Ring vs. Sender-Verlauf) ⇒ sie entstehen im
**Backfill-Merge**, **nicht** im CSV-Upload.

**Root Cause (host-reproduziert, 2026-10-14):** `main/power_history.c` nimmt an, der Ring
sei **zeitlich sortiert** — `minute_is_stored()` = **Binärsuche** über `oldest_slot..next_slot`,
ebenso der `power_history_read*`-Pfad. Der Backfill fügt **ältere** Minuten an ⇒ Ring wird
**unsortiert** ⇒ `minute_is_stored()` / `power_history_has_minute()` finden **vorhandene**
Minuten nicht mehr ⇒ `write_records()` hält sie für neu ⇒ **dieselbe Minute wird erneut
geschrieben**. Reproduktion: 10 Live-Minuten enqueuen, dann 10 **ältere** mergen →
`has_minute(T0)` = **0** (soll 1); dieselben Live-Minuten erneut mergen → `record_count`
20 → 30.

Fix (empfohlen **Option A**): Ring **zeit-sortiert halten** — beim Einfügen älterer
Backfill-Minuten die korrekte Slot-Position bestimmen und den Ring umschaufeln, sodass
`oldest_slot..next_slot` aufsteigend bleibt. Betroffen: `write_records()` (gemeinsamer Pfad
für Live-Recorder **und** Merge). Alternative B: `minute_is_stored` + `read*` auf
unsortierten Ring auslegen.

**Zwei zusätzliche Abnahmepunkte:**
- [ ] **Bereits unsortierten Bestand mitreparieren.** Nicht nur neue Inserts sortiert
      einfügen — der Ring ist am Gerät **schon** zerschossen. Vorhandene Daten einmalig in
      sortierte Ordnung überführen (z. B. Normalisierung beim Start), sonst bleiben
      `has_minute`/Export für die Alt-Daten kaputt.
- [ ] **`sequence` beim Umsortieren monoton halten.** Export/HA blättern über `sequence`
      (`power_history_read(after_sequence=…)`). Neu-Nummerierung macht alte Cursor
      ungültig ⇒ Export/HA verlieren Zeilen. `sequence` als Identität erhalten.

**Mitnehmen:**
- [ ] **Batch-Dedup im Backfill:** `history_sync_run` filtert nur gegen `has()`
      (schon gespeichert); dieselbe Minute kann **innerhalb** einer Session doppelt
      ankommen (Retry/überlappende Seiten) → vor `merge()` im Batch deduplizieren.
- [ ] **Backfill-`errors` („Fehler: 2")** im Sender-Abgleich: bei `fetch()`-Fehlschlag
      (`sender_diag_exchange_raw`) bricht die Seite ab, Cursor retryt später. Prüfen, ob
      Fehler gezählt/erneut versucht werden sollen.

**Regressionstest (Pflicht):** Host-Test in `test/` (analog `test_history_backfill_merge.c`):
(1) Live-Minuten enqueuen, ältere mergen; (2) `power_history_has_minute(T0) == true`, Ring
sortiert; (3) dieselben Minuten erneut mergen → `record_count` unverändert; (4) CSV-Export
liefert **keine** doppelten Minuten; (5) vorab unsortierter Ring + Export-Monotonie.
`test/run_host_tests.sh` grün; neuer Test eingehängt. OTA-sicher (reine App-Änderung).

### 🐛 UX: Interface minutenlang offline nach CSV-Upload (NEU, 2026-10-14)
Nach dem Re-Import einer großen CSV (`POST /api/power-history-upload`) ist die Weboberfläche
des Empfängers **minutenlang nicht erreichbar**. Der Handler rebootet **nicht** (kein
`esp_restart()`), sondern streamt den Body und reiht die Minuten ein. Vermutete Ursache: die
große Upload-Transaktion blockiert den HTTPD-Task; zusätzlich belastet die Dashboard-
Auto-Aktualisierung (`updateHistory` alle 1,5 s) den Server.
- [ ] Ursache eingrenzen (HTTPD-Task blockiert vs. Heap/SPIFFS-Druck vs. Watchdog).
- [ ] Upload in den Backfill-Worker verlagern bzw. HTTPD während des Imports nicht blockieren.

**2026-10-14 (Bau-Chat, Branch `cline/auftrag-history-dubletten`) — teilweise behoben:**
Der `AbortError`/„minutenlang offline" hat zwei Ursachen, beide jetzt entschärft:
- **HTTPD hing am Mutex.** Lesepfade (`power_history_read*`, `_aggregate`,
  `_has_minute`, `_get_status`) warten nur noch 800 ms auf `g_history_mutex`
  (`POWER_HISTORY_READ_LOCK_TIMEOUT_MS`) statt `portMAX_DELAY`; Writer/Import
  behalten `portMAX_DELAY`. Zusätzlich yielded `shift_up_renumber()` alle 8 Chunks
  (`vTaskDelay(1)`), damit Watchdog/Waiter laufen. Die Handler liefern bei
  `ESP_ERR_TIMEOUT` einen **leeren, gültigen** Datensatz statt HTTP 500/Hängen.
  Dateien: `main/power_history.c`, `main/main.c`.
- **Noch offen:** den Upload selbst in den Backfill-Worker verlagern, damit der
  Import den HTTPD-Task gar nicht erst lange belegt (die Mutex-Timeouts sind der
  Sicherheitsgurt, nicht die Wurzel). Siehe `AGENTS.md` → „Web-Reachability +
  'kein Datenverlust'".

### ✅ Kein Datenverlust beim Backfill (BEHOBEN, 2026-10-14)
In `history_sync.c` rückte der Resume-Cursor **vor** dem Merge vor: ließ der
Store-Puffer (16) einen Tail fallen, übersprang der Cursor diese Minuten ⇒
endgültig verloren. Jetzt rückt der Cursor nur über **nachweislich gespeicherte**
Minuten vor; bei einem Drop endet die Session vor der ersten fallengelassenen
Minute und die nächste zieht sie nach. Regressionstest
`test/test_history_sync.c` + Stress-Simulation `test/test_stress_history_sync.c`
(grün, 330 Checks). Details: `AGENTS.md`.

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

Flash-Reihenfolge (USB-Full-Flash, gilt fuer **beide** Rollen — identische
Partitionstabelle; `storage` nur beim **allerersten** Deployment mitschreiben):
```
0x1000   bootloader.bin
0x8000   partition-table.bin
0xf000   ota_data_initial.bin
0x20000  <role>-sml.bin        # receiver-sml.bin ODER sender-sml.bin
0x320000 storage.bin           # NUR erstes Deployment (leeres SPIFFS, 917504 B)
```
- Ohne `storage.bin` ist es ebenfalls okay: `power_history_init()` formatiert eine
  leere Partition beim ersten Boot (`format_if_mount_failed = true`).
- Im CI-Artefakt/`Firmware/Receiver`-Bundle ist `storage.bin` zwar enthalten, aber
  aus `flasher_args.json` **entfernt** → das Bundle schreibt die Historie nie
  (`e944e04`); `storage.bin` nur manuell bei Erst-Deployment auf 0x320000 schreiben.
- Sender alternativ per OTA (`http://192.168.4.1/update` → „Sender via ESP-NOW"),
  OTA beruehrt `storage` nie.

Offen / zu verifizieren:
- [ ] Gerät per Kabel komplett flashen (`storage` unberührt), Boot prüfen.
- [ ] OTA-Update-Zyklus testen (Accept + Rollback).
- [ ] Historie nach COM-Flash erhalten (`record_count > 0`).

## Übergabe

Stand: Branch `cline/power-history-diag`, alle Commits **gepusht** (HEAD `bfed7e7`,
`origin/cline/power-history-diag` folgt). PR #1 (`main` ← `cline/power-history-diag`)
ist offen und aktuell, CI für die letzten Pushes grün
(Run `38314938729`, 2m47s; PR-Check `38314945182`, 3m1s).

**Push-Status (gelöst):** Schreibzugang über `GITHUB_WRITE_TOKEN` (fine-grained PAT,
läuft 2026-11-11 ab). `origin`-URL trägt den Write-Token, `git push` funktioniert direkt;
`gh` liest weiterhin `GITHUB_TOKEN` → `GH_TOKEN=$GITHUB_WRITE_TOKEN gh …` setzen.

**Offen in dieser Session:** siehe die Blöcke oben
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
- [x] **Sender->Empfaenger-RSSI im Diagnose-Report ausweisen** — erledigt: der Empfaenger
      merkt sich im `receive_callback` das letzte `info->rx_ctrl->rssi` (`g_sender_rssi_dbm`)
      und zeigt es im neuen `/api/sender` (`sender_rssi`, `sender_rssi_valid`) sowie auf der
      `/sender`-Seite an. Der Sender selbst kann die Gegenrichtung nicht messen (er sendet nur).
- [ ] **Event-Zeitstempel als Uptime darstellen** statt als 1970-Datum.
      Grund: Der Sender hat keine Netzwerkzeit (SNTP laeuft nur im Empfaenger);
      `timestamp_ms` ist `esp_timer_get_time()/1000`.
      Betroffen: `main/main.c` -> `diag_page_handler` (JS rendert `new Date(e.timestamp_ms).toISOString()`).

### Sender-Site (Variante A: vom Empfaenger gehostet, ESP-NOW getunnelt)
Entscheidung 2026-10-14: **ESP-NOW ist kein IP** (Link-Layer, max. 250 B/Frame). Ein Browser
kann keine Seite "ueber ESP-NOW" oeffnen, und der Sender bleibt deshalb **WLAN-frei**
(kein STA, kein eigener Webserver). Stattdessen hostet der **Empfaenger** die Sender-Seite
und tunnelt jede Anfrage ueber den bestehenden Diag-Kanal (`sender_diag_exchange`) zum Sender.
- [x] **`/sender`-Landingseite** (`sender_page_handler`) — kombiniert Counters, letzter
      Crash/vorige Session, letzte Events, Sender->Empfaenger-RSSI, `build_profile` und
      Sleep-Status. Datenquelle: `GET /api/sender` (`sender_status_api_handler`, ein JSON aus
      Counters- + Crash-Pull; Events paged per `/api/sender-diag?section=events`).
- [x] **Web-Konsole fuer den Sender** (`POST /api/sender-console`, `X-OTA-Key`-geschuetzt):
      neuer Diag-Request-Typ `ESPNOW_DIAG_SECTION_CMD` (Wire-Structs
      `espnow_diag_cmd_{request,response}_t`). Der Sender fuehrt die Zeile lokal ueber den
      vorhandenen `web_console_run()` aus (status, time, wifi, reboot, …) und schickt die
      Ausgabe ueber ESP-NOW zurueck. `ok=0` -> HTTP 422.
- [x] **`max_uri_handlers`** 28 -> 31 (drei neue Endpunkte).
- [ ] **Bench-Test**: `/sender` gegen einen laufenden Sender pruefen (Counters/Crash/Events,
      RSSI-Vorzeichen, Konsolen-Kommandos inkl. `reboot`).
- [ ] **Konsolen-Ausgabe > 192 B** kuerzen oder paginieren (aktuell hart auf
      `ESPNOW_DIAG_CMD_OUTPUT_LEN` begrenzt).

### mDNS (Empfaenger)
- [x] **Empfaenger per Name erreichbar**: `espressif/mdns` (v1.14.0) in `main/idf_component.yml`,
      `start_mdns()` setzt `esp32-meter.local` und advertiset `_http._tcp`. Start bei
      `WIFI_EVENT_AP_START` und `IP_EVENT_STA_GOT_IP` (idempotent), Hostname
      `MDNS_HOSTNAME="esp32-meter"`. **Nur der Empfaenger** hat eine Netzwerkidentitaet.
- [ ] **Bench-Test**: `esp32-meter.local` aufloesbar (Avahi/Bonjour) und STA-IP im Log sichtbar.

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
      nur die Boot-Log-Zeile `Persistent history ready: N of 29000`. Echten
      SPIFFS-Status (Datei offen) melden, damit „bereit" nicht in die Irre fuehrt.

### MQTT / Home Assistant (geplant)
- [x] **Speicherdaten nach HA bringen** (ohne MQTT): `tools/ha_import.py` zieht die
      Rohdaten ueber `/api/power-history?after=<seq>`, aggregiert stuendlich
      (`tools/ha_stats.py`) und importiert sie per HA-WebSocket-Befehl
      `recorder/import_statistics` mit echten UTC-Zeitstempeln. Version-aware
      (`mean_type`/`unit_class` ab HA 2025.10). Stdlib-only WebSocket-Client
      (`tools/ws_min.py`). Tests: 28 + 16 + 7 Checks, keine Mocks.
- [x] **MQTT-Client im Empfaenger** (esp-mqtt; in IDF v6.1 managed Component
      `espressif/mqtt` + `espressif/cjson`, deklariert in `main/idf_component.yml`).
      Umgesetzt in `main/mqtt_bridge.{c,h}` (nur Empfaenger, per `#if`-Rolle):
      Broker-URI/User/Passwort/Topic-Prefix aus NVS (Namespace `mqttcfg`); ohne URI
      bleibt MQTT aus. Start bei `IP_EVENT_STA_GOT_IP`. Publish:
      - Live-Leistung: `<prefix>/power` (retained, JSON `{"power":<W>,"unit":27}`)
      - Abgeschlossener Minutenwert: `<prefix>/history` als JSON
        `{"t":<utc>,"w":<avg>,"n":<count>}`
      - Verfuegbarkeit: `<prefix>/status` (online/offline via LWT, retained)
      - Discovery-Hub: `<prefix>/state` (retained)
      Web-Konsole: Befehl `mqtt`. **CI gruen** (Run 38319709982, Receiver+Sender+Host).
      Grammatik/Format der Topics kommt aus `main/mqtt_topics.c` (host-getestet).
- [x] **Home-Assistant-Discovery** (`homeassistant/sensor/.../config`): als
      `mqtt_discovery_topic`/`mqtt_discovery_payload` in `mqtt_topics.c` implementiert
      und beim Connect retained veroeffentlicht; aktuell eine Entitaet „Leistung"
      (`device_class: power`, `state_class: measurement`, Unit W, mit Availability).
      **Offen:** weitere Entitaeten (Import/Export in kWh → `state_class: total_increasing`).
- [ ] Optional: Live-MQTT-Push der Minutenwerte, damit HA ab sofort selbst
      mitschreibt (dann ist der Import nur noch fuer die Alt-Daten noetig).
- **⚠️ Am Geraet noch zu verifizieren:** Broker in NVS setzen, dann `mqtt` in der
      Konsole pruefen und in HA sehen, ob die Entitaet auftaucht (LWT online/offline
      durch Broker-Neustart testen). Broker/Port/User/Pass sind Werte, die der
      Nutzer setzt (nicht im Code).

### Power-History-Dashboard (kosmetisch, optional)
- [x] **Balken-Wert per Hover/Tap einblenden**: Tooltip auf dem Canvas zeigt Wert + Zeitpunkt;
      Maus (`mousemove`) und Touch (`touchstart`/`touchmove`) unterstuetzt, funktioniert also
      auch am Touch-Display. Basis-Chart liegt in einem Offscreen-Canvas, beim Hervorheben
      wird nur geblittet + der Balken betont. JS-Syntax- und Laufzeit-Check in CI.
- [ ] `current_sample_count`-Label praezisieren (aktuell: 15-min-Ringpuffer, nicht Samples im aktuellen 60-s-Bucket).
- [ ] `resize`-Handler startet zusaetzliche 60-s-Schleifen (`updateHistory`) -> Mehrfach-Timer vermeiden.

### Diagnose-Ausbau (CPU-Takt, Sleep, Speicher, Sync) + Vergleich in allen Ansichten
Ziel: Die Energie-/Sync-Diagnose fuer **beide** Rollen sichtbar machen und den
Vortag-Vergleich aus der Tagesansicht heraus auf **alle** Zeitraeume (bis 24 h und
darueber) ausdehnen.

- [x] **Light-Sleep meldbar machen:** `CONFIG_PM_LIGHT_SLEEP_CALLBACKS=y` in
      `sdkconfig.defaults`, `esp_pm` in `main/CMakeLists.txt`; `sleep_enter_callback()`/
      `sleep_exit_callback()` setzen `sleep_state` und summieren `uptime_sleep_ms`.
      Ohne diese Callbacks wurden die Felder im Diag-Struct nie befuellt.
- [x] **Sender-Diag (`section=counters`):** `cpu_freq_mhz` (live `esp_clk_cpu_freq()`),
      `cpu_freq_max_mhz`/`cpu_freq_min_mhz` (DFS-Bereich), `light_sleep_enabled`,
      `sleep_state`, `uptime_sleep_ms`, plus Speicher (`storage_ready`, `storage_records`,
      `storage_capacity`, `storage_write_errors`, `storage_queue_drops`).
- [x] **Empfaenger-Diag:** eigenes Dashboard-JSON liefert CPU-Takt + Sleep; `GET /api/history-pull`
      liefert zusaetzlich `sessions`, `last_merged`, `last_pages`, `last_duration_ms`,
      `last_round_ok`, `lag_seconds` (Rueckstand des Resume-Cursors hinter der neuesten
      Sender-Minute).
- [x] **Dashboard-Zeilen:** Meterzeile zeigt `CPU: … MHz (DFS a-b MHz) · Sleep: …`;
      neue Zeile **„Sender-Abgleich"** unter dem Diagramm (Rueckstand, Sitzungen, zuletzt
      uebernommen, Dauer, Fehler, Sender-Speicherbereich, Cursor).
- [x] **Tooltip zeigt beide Werte:** Im Vergleichsmodus findet `showTip()` pro Serie den
      naechsten Balken und listet **Aktuell und Vortag** mit Farbmarkierung + einer
      Δ-Zeile (Differenz zum Vortag). `plot()` taggt jeden Eintrag mit `srcT`/`label`/`swatch`.
- [x] **Vergleich in jeder Ansicht (bis mind. 24 h):** Der Vergleich wird jetzt auch im
      **Live-Modus** geladen. Neuer Server-Parameter `start` (Epoch-Sekunden) verankert das
      Aggregat-Fenster; der Empfaenger fragt das Vortags-Fenster `start-86400` ab und
      verschiebt es mit `compareShift` (+1 Tag) slotgenau aufs sichtbare Fenster. Der
      „Vortag vergleichen"-Button schaltet nicht mehr zwangsweise in die Tagesansicht um,
      der gewaehlte Zeitraum bleibt erhalten.
- [x] **Host-Tests:** `test/check_dashboard_js.py` prueft jetzt zusaetzlich
      „dashboard compare tooltip ok" (beide Serien + Δ im Tooltip) und
      „dashboard live compare shift ok" (Live-Vergleich landet im Fenster).
- [ ] Am Geraet verifizieren: Live-Slider auf 6/12/24 h + „Vortag vergleichen" -> gelbe
      Kurve + Tooltip mit beiden Werten.

### ✅ Behoben: Button „Vortag vergleichen“ tat in der Live-Ansicht nichts
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
- [x] **Fix 1** (`d0ef9e9`): `setLive(false)` in `toggleCompare()` vor `redraw()` ergänzt.
      (z. B. `setLive(false);`), dann wird der Vortag (gelb) über den Tagesverlauf gelegt.
- [x] Optional: der Slider-Handler (`history-range`) setzt `historyCompare` jetzt zurück.
      (der Slider-Handler `history-range` tut das derzeit nicht).
- [x] **Fix 2** (der eigentliche, tiefere Grund, dass es „immer noch nicht“ ging): Die
      geladenen Vergleichswerte tragen die Zeitstempel des **Vortags**, aber `plot()` filtert
      `t<start||t>end` gegen das **gewählte** Tagesfenster. Dadurch wurden **alle**
      Vergleichsbalken geclippt — der Button sah tot aus, obwohl er korrekt umschaltete.
      Fix in `drawHistory()`: `dayAlign = start − Date.parse(historyCompare+'T00:00:00Z')`
      und `plot(compare, '#e2b53e', dayAlign)` verschiebt den Vortag auf das gewählte
      Fenster (Slot→Slot). Regressionstest in `test/check_dashboard_js.py`
      („dashboard compare alignment ok“; rot ohne `dayAlign`, grün mit).
- [ ] Verifizieren am Gerät: Live → Button klicken → gelbe Vergleichskurve +
      Info „Vergleich &lt;Datum&gt; (gelb)“ sichtbar (gilt für den gewählten Tag = heute
      wie für zurückgeblätterte Tage; Vortag im Fenster ist slotgenau kompatibel).

### 🐛 Behoben: wiederkehrende „Schreibfehler" (Ringpuffer größer als die Partition)
`POWER_HISTORY_CAPACITY` war **30000** (→ Datei 56 + 30000·28 = 840 056 B ≈ 820 KiB).
Die `storage`-SPIFFS-Partition ist aber nur 896 KiB groß und SPIFFS lässt für die
Garbage Collection Reserve: auf dem Host mit `spiffsgen.py` (ESP-IDF v6.1) nachgerechnet
passen nur **≈ 29 600** Datensätze hinein. Der Ringpuffer konnte seine Wrap-Grenze daher
**nie** erreichen: ab ~29 600 Datensätzen (≈ **20,6 Tage Dauerbetrieb**) lief jeder
Minuten-Schreibvorgang auf `SPIFFS_ERR_FULL`, die Datei wuchs nicht weiter und das
Dashboard zeigte jede Minute „Schreibfehler: N". Ein Ringpuffer muss beim Vollstand
überschreiben — das war durch die zu große Kapazität unmöglich.
- [x] `POWER_HISTORY_CAPACITY` auf **29000** gesenkt (~20,1 Tage, 4 reservierte Blöcke).
      Überschreib-Logik (`next_slot = (next_slot+1) % CAPACITY`) war korrekt und ist jetzt
      erstmals erreichbar.
- [x] Dashboard nutzt `d.capacity` statt der fest verdrahteten „/ 30000" (beide
      JSON-Header liefern `"capacity"`).
- [x] Host-Suite grün (`./test/run_host_tests.sh`, inkl. 61 Stability-Checks); kein Test
      hing an 30000.
- Hinweis am Gerät: Nach dem Update zeigt das Dashboard erst bei `record_count` nahe der
      neuen Grenze wieder Schreiben. Einmal `POST /api/reboot` (oder Stecker) und danach
      „Schreibfehler" beobachten.



## Übernahmen aus großen externen SML-Projekten (Recherche 2026-10-12)

Bewertung anderer SML-Projekte auf GitHub. Wir nutzen den Parser bereits
(`olliiiver/sml_parser`, LGPL-2.1, in `components/sml_parser`), hier geht es um
das **Drumherum**. Reihenfolge nach Nutzen/Aufwand (schnell → kompliziert).

**⚠️ Lizenz-Falle zuerst:** `SMLReader`, `sml2mqtt`, `libsml` (volkszaehler) und
`vzlogger` sind **GPL-3.0** → nur Konzepte/Lehren übernehmen, **keinen Code
kopieren** (sonst wird das ganze Projekt GPL). `lrswss/esp32-sml-multi-reader`
ist **Apache-2.0** (Code mit Namensnennung erlaubt). Lizenz des Projekts noch
offen (s. u.) — **vor** dem Kopieren fremden Codes entscheiden.

- [ ] **Lizenz + README festlegen** (blockiert Code-Übernahme, ~30 Min).
- [ ] **CRC-Varianten (schnell, Lifestyle-Lehre aus `spacemanspiff2007/sml2mqtt`):**
      Nicht jeder Zähler nutzt den Standard-SML-CRC. Der **Holley DTZ541** verlangt
      die **Kermit-CRC** (Reflected 0x8408); sml2mqtt wählt die CRC automatisch.
      Unser Parser nutzt genau die Standard-Tabelle (`smlCrcTable.h`, Reflected
      0x8408) → die meisten Zähler ok, Holley bräuchte einen Umschalter.
      Prüfen/anmerken, keinen Blind-Fix. Betrifft `components/sml_parser`.
- [ ] **SML-Golden-Fixtures (schnell):** `test/fixtures/sml/*.bin` mit echten
      Telegrammen + Decode-Golden-Test. Erste Fixture host-seitig aus dem Parser
      erzeugen (nicht byte-copy), Zielwerte hart prüfen. Bestehende Randfall-Fixture
      (ABER-CRC): `smlOBISCheck` liefert `true` (Struktur dekodiert), die
      Telemetrie gilt wegen `crc_valid=false` als ungültig — gegen bestehendes
      Produktionsverhalten (Adapter/Transport) absichern. Telegramm-Referenz/Inspiration:
      `olliiiver/sml_parser`-Tests, `volkszaehler/libsml`-Frames (nur Konzept).
- [x] **MQTT-Topic-Schema + HA-Discovery-Logik (mittel):** Vorbild `mruettgers/SMLReader`
      (GPL-3.0, nur Konzept): pro OBIS-Code ein eigenes Topic statt Sammel-JSON,
      z. B. `<prefix>/sensor/<n>/obis/1-0:16.7.0*255/value`. **Fertig als reine,
      host-testbare Builder**: `main/mqtt_topics.{c,h}` (`mqtt_topics_build`,
      `mqtt_topic_obis`, `mqtt_discovery_topic`, `mqtt_discovery_payload`), Test
      `test/test_mqtt_topics.c` (30 Checks, im Runner aktiv). Noch **nicht** an
      esp-mqtt angeschlossen (nächster Block).
- [ ] **MQTT over TLS (kompliziert, optional):** Vorbild `lrswss/esp32-sml-multi-reader`
      (Apache-2.0): esp-mqtt mit TLS + optionaler Auth. Nur wenn der Broker
      abgesichert angebunden werden soll.
- [ ] **Aggregation beim Lesen statt nachträglich (Lehre aus `vzlogger`
      `aggtime`/`aggmode`):** bestätigt den Plan „Tages-/Monats-Summen im Flash";
      reduziert Schreiblast/Flash-Verschleiß erheblich (s. o., AMS-Punkt).
- _Nicht übernehmen:_ MicroPython-Parser (`SmlParserLight`), Kamera/OCR-Projekte,
      ESPHome-Komponenten (wir haben eigenes ESP-IDF-Firmware) — passt nicht.



## Erledigt (in dieser Session)
- **Web-Health-Guard (2026-10-14, gegen „Web offline, kein Rollback").** Neues,
  host-getestetes Modul `main/web_guard.{c,h}` (`test/test_web_guard.c`, in
  `run_host_tests.sh`): Receiver-only Task pingt alle 5 s `/healthz` per
  Loopback; nach 30 s Stille `httpd_stop`+`httpd_start`+Handler-Neuregistrierung,
  nach 90 s ohne Erholung `esp_restart()` (→ Boot-Policy greift wieder). Guard
  bleibt **passiv**, bis der Selbst-Probe einmal klappt (`probe_works`) — kein
  Reboot eines gesunden Geräts ohne funktionierenden Loopback. `status` zeigt
  `web health: guarded/passive, restarts N`. `max_open_sockets=10`.
  **Hardware-Verifikation offen** (Loopback-Probe + Restart am Gerät prüfen).
- **Versionsnummern (2026-10-14, `cline/auftrag-history-dubletten`).** Root-CMake
  löst die Version aus `$FIRMWARE_VERSION` (CI) bzw. `git describe` auf und
  schreibt die `main/build_version.h` (`FIRMWARE_VERSION`); `main/version.c`
  liefert sie als `fw_version()` (Fallback `PROJECT_VER`/`"unknown"`). Gemeldet
  als JSON `version` in `/api`, `/api/dashboard`, `/api/sender` und den
  Sender-Diag-`counters`, sichtbar auf der Dashboard-Statuszeile (`· FW: …`) und
  in Console/Diag (`fw: …`). CI schreibt `firmware-version.txt` ins Flash-Bundle,
  in die Step-Summary und prüft die Versionszeichenkette im gebauten `.bin`.
  Host-Test `test/test_version.c`. **Gotcha:** der Container-Build läuft als root
  gegen einen Checkout mit anderem Owner → `git` verweigert
  („dubious ownership"), Version wäre `unknown`; der Workflow setzt daher
  `safe.directory $GITHUB_WORKSPACE` und exportiert `FIRMWARE_VERSION`.
  Release-Tags (`vX.Y.Z`) liefern saubere Versionen statt nackter SHAs.
- **Diag-Sichtung (Sender/Receiver) + zwei Bugfixes**: „DID NOT END CLEANLY" bei bewusstem
  Reboot behoben (deliberate-restart-Klassifizierung, Sender-Diag), Live-Dashboard zeigte nur
  15-Min-Balken (Sekunden/ms-Einheitenfehler im Live-Fenster, Receiver) behoben; dazu der
  Sender-Light-Sleep als einziges Sender-Image (Sleep Default AN) für den Bench-Test.
  Details in den gleichnamigen Abschnitten „🐛 … (BEHOBEN)" oben.
- **Adaptiver Sender-Light-Sleep + Hold für Daten/OTA** (`2c2126c`, `0b9f637`): Regler
  `uart_sleep_window.{h,c}` (400 ms Start, Auto-Backoff, Lock hält den Burst wach),
  `uart_sleep_hold()` gegen Sleep während History-Pull/OTA, Sim + Unit-Tests (21+14 Checks),
  Diag-Felder `sleep_window_ms`/`sleep_min_gap_ms`/`sleep_hold_active`, CI-Syntax-Check des
  aktivierten Zweigs. Feature ist jetzt Default **AN** (ein Sender-Image);
  Hardware-Verifikation offen (s. o.).
- **Receiver-Build nach `2536550` wieder grün** (`fee4c43`): `console_cmd_pull()` rief
  `history_backfill_run_session()` vor der Definition auf; ohne Vorwärtsdeklaration
  scheiterte der Receiver-Build an `-Werror=implicit-function-declaration`
  (Sender + Host-Tests waren grün). Prototyp ergänzt, CI wieder success.
- **SML-Golden-Fixture + CRC-Hinweis** (`3427bd2`): echtes eHZ-Telegramm als Fixture,
  Golden-Decode-Test (8 Checks, ASan+UBSan), Standard-CRC/Kermit-Caveat dokumentiert.
- **MQTT-Topic-/Discovery-Builder** (`ee8ea13`): `main/mqtt_topics.{c,h}`, host-getestet
  (30 Checks) — reine Logik, Vorbild SMLReader-Topic-Schema.
- **esp-mqtt-Bridge im Empfänger** (`b76c7d7` + Fix `c5bf82c`): `main/mqtt_bridge.{c,h}`,
  managed Components `espressif/mqtt`+`espressif/cjson`, Live-Leistung/Minutenwert/LWT/
  HA-Discovery, Console-Befehl `mqtt`. **CI grün** (Run 38319709982).
- **Externe SML-Projekte gesichtet** (Recherche/Übernahmen, s. o.).
- **Tagesgrenzen im Chart exakt um 0:00 UTC** (`e7e7cb7`): `drawHistory()` zeichnet bei
  `hours >= 24` gestrichelte Linien auf jedes echte UTC-Mitternacht
  (`Math.ceil(start/86400000)*86400000`) statt auf beliebige Rasterpositionen; der
  Host-Test `test/check_dashboard_js.py` erzwingt das (rot ohne Fix, grün mit Fix).
- **Wiederkehrende „Schreibfehler" behoben** (`af88bbd`): Ringpuffer-Kapazität
  30000 → 29000, weil 30000 Datensätze (840056 B) nicht in die 896-KiB-`storage`-Partition
  passen — der Puffer konnte nie überschreiben und lief ab ~29600 Sätzen auf
  `SPIFFS_ERR_FULL`. Details oben im Abschnitt „🐛 Behoben".
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
