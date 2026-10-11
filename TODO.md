# To-do – esp32-IR

Stand: Branch `cline/power-history-diag` (Basis: On-Demand-Diagnose + Leistungshistorie).

## Offen

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

### Bedienung / Diagnose (neu)
- [ ] **Reboot-Taste im Dashboard** (`POST /api/reboot`, Auth-Gate wie `/update`,
      Antwort senden, dann `esp_restart()`). Nuetzlich fuer den Persistenz-Test
      (Reboot ohne Stecker ziehen) und generell zum Neustart aus der Weboberflaeche.
      Muss auf der AP-Seite auch ohne STA-Verbindung funktionieren.
- [ ] **Debug-Konsole mit Eingabe** (seriell und/oder Web): Befehle senden und
      Antworten sehen, z. B. `status`, `history status`, `nvs dump`, `reboot`,
      `sntp resync`, `espnow diag`. Der Empfaenger hat `esp_console` noch nicht
      verdrahtet; UART ist belegt (SML-Eingang) -> nur RX/TX-freie Kanaele nutzen
      oder Web-Console ueber `/api/console`. Beruehrt keine geschuetzten Bereiche.

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
- [ ] **MQTT-Client im Empfaenger** (ESP-IDF `mqtt`/esp-mqtt). Broker/Port/Benutzer/Passwort
      + Topic-Prefix in NVS (bestehendes `metercfg`-Namespace erweitern). LWT-Topic fuer
      online/offline, Publish von:
      - Live-Leistung: `<prefix>/power` (W)
      - Abgeschlossener Minutenwert: `<prefix>/history` als JSON
        `{"t":<utc>,"w":<avg>,"n":<count>}`
      - Verfuegbarkeit: `<prefix>/status` (online/offline via LWT)
      Nur Empfaenger (hat STA/Netz); ESP-NOW, OTA und Rollen bleiben unberuehrt.
- [ ] **Home-Assistant-Discovery** (`homeassistant/sensor/.../config`), damit die Entitaeten
      automatisch erscheinen: `device_class: power`, `state_class: measurement`, Unit W,
      plus Energie-Zaehler (kWh) fuer das Energie-Dashboard.
- [ ] **Speicherdaten nach HA bringen**: HAs Recorder fuellt per MQTT **nicht** rueckwirkend
      auf. Optionen (mit Nutzer waehlen):
      - **Replay**: beim Connect die gespeicherten Minutenwerte einmalig publizieren
        (begrenzte Burst-Anzahl), plus Web-Konsolen-Befehl `mqtt replay N`.
      - **Langzeit-Statistik-Import** in HA aus dem Replay (`recorder.import_statistics`
        bzw. Statistik-Import) — exakte API vor Umsetzung verifizieren.
      - **REST-Sensor** in HA, der `/api/power-history?day=...` (CSV/JSON) zieht und
        rueckwirkend eintraegt.
- [ ] **Web-Konsolen-Befehle**: `mqtt status`, `mqtt reconnect`, `mqtt replay N`.

### Power-History-Dashboard (kosmetisch, optional)
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
