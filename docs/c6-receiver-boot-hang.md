# C6-Receiver: Boot-Hang vor `start_webserver()` - Diagnose + Haertung

**Branch `feat/c6-mqtt-cmd`.** Reine Repo-Arbeit, kein Kompilieren auf dem Odroid
(GitHub-CI). Host-Tests lokal gruen.

## Symptom (belegt, live am C6 `192.168.178.105`)

- **Port 80 tot**: `nmap` sieht alle TCP-Ports `closed (reset)` - der `httpd`
  laeuft nicht, aber das Geraet antwortet (RST), ist also **nicht** aus.
- **Kommando-Registry leer**: `{"cmd":"help","ok":false,
  "out":"unknown command 'help' - type 'help'\n"}` ueber den MQTT-`cmd`-Kanal.
- **MQTT lebt**: `esp32ir/receiver/status` = `online`, `.../cmd/result` kommt
  zurueck.
- **Ping lebt**, aber mit `3 ms <-> 546 ms` (Modem-Sleep aktiv).
- **`esp32ir/receiver/power` steht still** (retained `131.0`, kein Update in 15 s)
  -> der 1-Hz-`power_sample_timer` laeuft nicht.

## Beweiskette

1. **Die Kommando-Registry ist reiner RAM** (`main/web_console.c`,
   `g_commands[]`). Sie wird **nur** in `register_webserver_handlers()`
   (`main.c`, `web_console_register_commands()` @ ~9356) bzw. `run_sender()`
   befuellt - beide laufen **am Ende** von `start_webserver()` bzw. des
   Senderpfads. Eine leere Registry heisst daher zwingend: **dieser Boot hat
   `start_webserver()` nie erreicht** (nicht "Server gestartet, dann gestorben").
2. **Der Power-Sample-Timer (1 Hz)** wird in `app_main` **vor**
   `start_webserver()` erzeugt und gestartet. Ein stiller `.../power`-Topic
   heisst: `app_main` steht **vor** dem Timer-Start bzw. der Timer wurde nie
   bedient.
3. **MQTT/Ping leben weiter**: `mqtt_bridge_start()` laeuft im
   `IP_EVENT_STA_GOT_IP`-Handler auf dem `sys_evt`-Task, **nicht** in `app_main`.
   Ein `app_main`-Hang laesst `sys_evt` (und damit MQTT) intakt - genau das
   Muster.
4. **Watchdog-Luecke**: `liveness_guard_start()`, `health_task_start()` und
   `web_guard_start()` starteten **alle erst nach** `start_webserver()`. Ein
   Hang irgendwo in `app_main` **davor** war damit **permanent** (kein Reboot).
   Der einzige beobachtete Reboot (`task-watchdog`, 120 s) kam aus einem
   *spaeteren* Zustand, nicht aus dem Init-Hang.
5. **Boot-Crash-Loop am 2026-11-14 ~11:33** (Mosquitto-Log: Reconnect alle
   ~7,5 s) weist auf einen `abort()` im Boot hin (vgl. `95f3c73`,
   `ESP_ERR_HTTPD_HANDLERS_FULL`). Danach stabil-aber-tot = ein Boot, der
   **vor** `start_webserver()` haengen blieb.
6. **USB-Korrelation** (`dmesg`): der C6 (`idVendor 303a:1001`,
   `SerialNumber 10:BD:A3:88:D7:14` == MQTT-Client `ESP32_88D714`) war als
   `ttyACM0` enumeriert und wurde um ~12:17 **abgezogen**
   (`usb 1-1.3: USB disconnect`) - exakt im Fenster, in dem HTTP/Konsole starben.
   Die C6-Konsole **ist** die USB-Serial/JTAG-Peripherie; ein Abziehen mitten im
   Boot ist ein plausibler Ausloeser fuer einen blockierten Init-Pfad.

## Was der Hang **nicht** ist

- **Nicht** der ROM-Logpfad: `usb_serial_jtag_tx_char_no_driver()` **verwirft**
  Ausgabe nach `TX_FLUSH_TIMEOUT_US = 50 ms`, wenn kein Host liest - es
  blockiert **nicht** ewig (IDF v6.1
  `components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c`).
- **Nicht** die `web_guard`-Selbstheilung: die greift erst **nach** dem Boot und
  kann einen Hang **vor** `start_webserver()` nicht sehen.

## Haertung (dieser Branch)

Zwei kleine, **verhaltensneutrale** Umstellungen in `app_main` plus ein
CI-Source-Guard:

1. **`liveness_guard_start()` ganz an den Anfang von `app_main`** (vor
   `boot_log_init`/`initialize_wifi_and_espnow`). Der Backstop ist damit **immer**
   scharf; `liveness_note_alive()` wird nach jeder Init-Phase gefuettert
   (Wi-Fi-Init, `task_wdt_guard_init`, `power_history_init`, `energy_totals_start`,
   Recovery-Check, nach `start_webserver`). Ein Hang in **irgendeiner** Phase
   fuehrt jetzt nach der 90-s-Grace + Eskalation zu einem **Reboot** statt zu
   einem Dauerhaenger. Der Guard wird nicht mehr doppelt gestartet.
2. **`web_console_register_commands()` ganz am Anfang von `app_main`** (vor der
   Wi-Fi-Init). Die Registry ist RAM-only und war bisher nur am Ende von
   `start_webserver()` befuellt. Jetzt antwortet die USB-Serial-Konsole
   (`help`, `status`, `reboot`, ...) **auch dann**, wenn der Webserver nie
   hochkommt - das zweite Symptom ist damit entschaerft. Idempotent
   (`web_console_register` aktualisiert vorhandene Eintraege), der spaetere Aufruf
   in `register_webserver_handlers()`/`run_sender()` bleibt unveraendert.

   Effekt: Ein `reboot` per USB-Konsole ist ab dem ersten Moment moeglich - die
   wichtigste Rettungsleine, wenn der Webserver tot ist.

3. **CI-Guard** `Verify the receiver boot-order guard (liveness + console first)`
   in `.github/workflows/esp-idf-build.yml`: prueft per Zeilennummern (in
   `app_main` gescoped), dass `liveness_guard_start()` **vor**
   `start_webserver()` und `web_console_register_commands()` **vor**
   `initialize_wifi_and_espnow()` steht. Build-only-CI kann das Laufzeit-Symptom
   nicht sehen, den Source-Invarianten aber schon.

## Restrisiko / offen

- Die **exakt blockierende Zeile** ist nicht mit letzter Sicherheit benannt
  (Kandidaten: ein NVS/SPIFFS-Aufruf, ein `esp_pm_configure`-Pfad, ein
  USJ-Interaktionspfad waehrend des Detach). Die Haertung macht den Hang
  **beobachtbar und selbstheilend** (`liveness`-Reboot + funktionierende
  Konsole), statt ihn stillschweigend dauerhaft zu machen.
- Sobald die Ursache reproduzierbar ist, kann die Zeile gezielt gefixt werden;
  bis dahin ist der Backstop die Sicherheitsleine.
- Der eigentliche C6-Zielzustand (sparsam, stabil) bleibt: `receiver-c6-apoff`
  fuer Idle-Strom, SoftAP nur fuers Erst-Setup (`docs/c6-idle-power-measured.md`).
