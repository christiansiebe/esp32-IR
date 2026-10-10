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
- [ ] **`storage.bin` in das CI-Flash-Artefakt aufnehmen** (`.github/workflows/esp-idf-build.yml`).
      Blockiert: Das Token der GitHub-App "Cline Cloud" hat keine `workflows`-Berechtigung
      (Push/API liefern: `without 'workflows' permission` bzw. 403 `Resource not accessible by integration`).
      Patch im Schritt "Stage firmware and flash files":

      ```diff
                 test -s "$build_dir/ota_data_initial.bin"
      +          test -s "$build_dir/storage.bin"
                 cp "$build_dir/ota_data_initial.bin" "$out_dir/ota_data_initial.bin"
      +          cp "$build_dir/storage.bin" "$out_dir/storage.bin"
      ```

      Bis dahin: Patch selbst anwenden, oder lokal `idf.py flash`
      (`FLASH_IN_PROJECT` nimmt `storage.bin` automatisch mit).
      Funktional unkritisch: `format_if_mount_failed = true` formatiert notfalls einmalig still.

### Power-History-Dashboard (kosmetisch, optional)
- [ ] `current_sample_count`-Label praezisieren (aktuell: 15-min-Ringpuffer, nicht Samples im aktuellen 60-s-Bucket).
- [ ] `resize`-Handler startet zusaetzliche 60-s-Schleifen (`updateHistory`) -> Mehrfach-Timer vermeiden.

## Erledigt (in dieser Session)
- On-Demand-Senderdiagnose ueber ESP-NOW (unicast, Request/Response, echte Sendebestaetigung via Callback).
- Persistente Leistungshistorie (SPIFFS/RAM-Fallback, SNTP, 1-s-Sampling, 60-s-Buckets, `/api/power-*`).
- SPIFFS-Robustheit: `format_if_mount_failed = true` + vorformatiertes Image (`spiffs_create_partition_image`).
- Hardware-Nachweis Diagnose (Sender): 2530/2530 Sendungen bestaetigt, 0 Drops, 0 UART-Fehler, 1 Diag-Request.
