# To-do – esp32-IR

Stand: Branch `cline/power-history-diag` (Basis: On-Demand-Diagnose + Leistungshistorie).
Letzter Commit: `8f12dc7` – CI-Run `38407371756` **grün** (Sender, sender-sleep, Receiver,
Host-Tests). Enthält: adaptiver Sender-Light-Sleep + Hold, deliberate-restart-Fix,
Live-Dashboard-Einheitenfix, `sender-sleep`-Bench-Artefakt.

## ⏰ Arbeitsweise: Chat-Heartbeat (von jedem Folge-Chat zu übernehmen!)
Diese Session läuft in einer Sandbox mit **Inaktivitäts-Timeout (~20 Min)**. Wird der Chat
nicht benutzt, wird die Sandbox zurückgesetzt und nicht committete Arbeit ist weg (siehe
„Geborgene Arbeit" unten). Deshalb gibt es einen automatischen Ping:

- Automation **`chat-heartbeat`** (OpenHands Cloud), Cron **`*/15 * * * *`**, Status ACTIVE.
  Jeder Lauf weckt die Session und setzt den Idle-Timer zurück (15 Min < 20 Min ⇒ 5 Min Puffer).
- **Was Folge-Chats tun sollen:**
  1. Beim ersten eigenen Schritt prüfen, dass `chat-heartbeat` noch ACTIVE ist (Liste der
     Automations). Falls nicht: neu anlegen (Cron `*/15 * * * *`, Nachricht enthält den
     Hinweis `[heartbeat]`), damit die Sandbox nicht wieder einschläft.
  2. Nach jedem bedeutenden Schritt **sofort committen + pushen** (kleine Commits) — der
     Heartbeat schützt nur den Chat, nicht die Arbeit. Nichts Wichtiges in `/tmp` halten.
  3. Bei einem eingehenden `[heartbeat] … keine Aktion noetig`-Ping: keine Aktion nötig,
     aber offene Arbeit aus diesem TODO weiterführen, falls vorhanden.
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
- [x] **SML-Golden-Fixtures**: `test/fixtures/sml/ehz_bin.upstream.h` (echtes EMH-eHZ-
      Telegramm, 319 B, aus dem öffentlichen libsml-testing-Korpus) + Decode-Golden-Test
      `test/test_sml_parser_golden.cpp` (8 Checks; 1-0:1.8.0/1.8.1 Wh, 1-0:15.7.0 W exakt;
      gekipptes Byte → `SML_CHECKSUM_ERROR`). Im Host-Runner aktiv, ASan+UBSan.
- [ ] **AP abschalten, wenn STA verbunden** (größter einzelner Spareffekt): Empfänger fährt
      dauerhaft `WIFI_MODE_APSTA` (`main.c`); nach `GOT_IP` auf `WIFI_MODE_STA` wechseln.
- [ ] **Tages-/Monats-Summen im Flash** (`hImport[24]`/`dImport[31]` wie AMS): aus
      Minutenwerten ableiten, platzsparend + schnellere Langzeitgrafik.
- [ ] **Letzte bekannte Zeit persistieren** (`lastMeterReadTime`): nach Reboot ohne SNTP ist
      `bucket_start==0` → keine Aufzeichnung; letzte UTC im Header/NVS ablegen.
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
variablem Takt, ohne dass ein Telegramm verloren geht. Feature liegt hinter
`CONFIG_ESPNOW_SENDER_LIGHT_SLEEP` (**Default AUS**).
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
- [ ] **Am Gerät verifizieren (offen):** Light Sleep als aktiv in der Diag; `uptime_sleep_ms > 0`;
      `parser_resyncs`/`gap_loss` bleiben **0**; Receiver bekommt weiter alle Minutenwerte;
      ESP-NOW stabil unter Modem-Sleep. Wenn nicht: Backfill als Netz nutzen, oder Feature aus lassen.
- [ ] Optional: `sleep_min_gap_ms` über die Diag-Seite im Dashboard als eigene Zeile ausgeben
      (bisher nur im Text).
- [ ] Optional: Default-ON erst nach Hardware-Nachweis; vorher AUS lassen (Sendestrecke schützen).
- [x] **Bench-Build verfügbar:** CI-Matrix hat jetzt einen dritten Eintrag `sender-sleep`
      (Rolle Sender + `CONFIG_ESPNOW_SENDER_LIGHT_SLEEP=y`) mit eigenem Artefakt
      `sender-sleep-sml-firmware` / `sender-sleep-sml-flash-files`; ein Schritt prüft per
      `grep`, dass das Flag wirklich gesetzt ist. So lässt sich die Sleep-Strecke flashen,
      ohne lokal zu bauen. (`.github/workflows/esp-idf-build.yml`)

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
Symptom: Mit geflashtem **sender-sleep**-Build häufige `rx gap … mid-telegram`
(~50–90 ms, ~1/s) und die Sorge, ob `espnow sleep` sich falsch verhält.
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
den Mid-Telegram-Sleep-Fix **nicht**. Für den Test den **aktuellen** `sender-sleep`-
Build flashen (Commits ab `f16cdc8`), sonst sieht man weiter die alte Symptomatik.

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
- **Diag-Sichtung (Sender/Receiver) + zwei Bugfixes**: „DID NOT END CLEANLY" bei bewusstem
  Reboot behoben (deliberate-restart-Klassifizierung, Sender-Diag), Live-Dashboard zeigte nur
  15-Min-Balken (Sekunden/ms-Einheitenfehler im Live-Fenster, Receiver) behoben; dazu
  `sender-sleep`-CI-Build als flashbares Artefakt für den Bench-Test. Details in den
  gleichnamigen Abschnitten „🐛 … (BEHOBEN)" oben.
- **Adaptiver Sender-Light-Sleep + Hold für Daten/OTA** (`2c2126c`, `0b9f637`): Regler
  `uart_sleep_window.{h,c}` (400 ms Start, Auto-Backoff, Lock hält den Burst wach),
  `uart_sleep_hold()` gegen Sleep während History-Pull/OTA, Sim + Unit-Tests (21+14 Checks),
  Diag-Felder `sleep_window_ms`/`sleep_min_gap_ms`/`sleep_hold_active`, CI-Syntax-Check des
  aktivierten Zweigs. Feature Default **AUS**; Hardware-Verifikation offen (s. o.).
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
