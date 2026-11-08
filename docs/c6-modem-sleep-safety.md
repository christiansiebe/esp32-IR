# C6-Empfänger: ein Modem-Light-Sleep, der nirgendwo Probleme macht

Stand: 2026-11-08 · Branch `feat/nvs-web-knobs` · am Laptop-Buildhost
(192.168.178.106) gegen ESP-IDF v6.1 gebaut, Host-Suite gruen.

## Das Problem in einem Satz

Der C6-Empfänger hat drei Stromspar-Hebel, und **jeder einzelne kann den Link
zerstören**, wenn man ihn falsch einsetzt:

| Hebel | Wirkung | Risiko |
|---|---|---|
| Light Sleep (`set sleep on`) | CPU/APB parken | **killt die USB-Serial/JTAG-Konsole** (C6) → Gerät über USB unerreichbar, USB-Bootloop |
| Modem-Sleep `ps min/max` (`esp_wifi_set_ps`) | Station verschläft DTIM-Beacons | **Disconnect-Sturm** (`reason 34` MISSING_ACKS), wenn der AP die Beacons nicht zuverlässig liefert oder `listen` zu groß ist |
| `radio-save` (`esp_now_set_wake_window`) | ESP-NOW-Funk parkt zwischen Fenstern | verpasste Sender-Frames, wenn `wake-interval` zu groß |

Dazu kommt die Umgebung: die FRITZ!Box (Mesh) **flappt auch ohne** jeden Hebel
(`reason 34`, RSSI teils −82 dBm). Ein Watchdog, der *jeden* Disconnect zählt,
würde also den Sleep grundlos abschalten.

## Die Lösung

### 1. Light Sleep bleibt am C6 hart aus (bereits umgesetzt, `7e34996`)
`runtime_config_light_sleep_allowed()` lehnt Light Sleep auf USB-Serial/JTAG-
Builds ab, `apply_power_management()` hält zusätzlich einen
`ESP_PM_NO_LIGHT_SLEEP`-Lock. Der NVS-Blob-Bump verwirft einen alten, kaputten
Wert (Selbstheilung). Das ist unverändert gültig.

### 2. Modem-Sleep ist opt-in und wird vom `ps_fallback`-Wächter abgesichert (neu)
`main/ps_fallback.{c,h}` + `ps_fallback_worker` (beide Rollen) zählen STA-
Disconnects in einem gleitenden Fenster. Ein Sturm (Default: **3 in 120 s**,
nach 30 s Boot-Schonfrist) heißt: der Modem-Sleep bricht den Link. Dann fällt der
Wächter **sticky** auf den bewährten Modus zurück und persistiert ihn:

```
cfg->wifi_ps_mode = 0   (WIFI_PS_NONE)
cfg->radio_save   = false
esp_now_set_wake_window(0); esp_wifi_set_ps(WIFI_PS_NONE);
```

Sticky, damit es nicht flappt (PS aus → Link heilt → PS an → wieder Disconnects).
Ein **absichtlicher** Reconnect (`set ps`/`set wifi-*`/`reset-config`) zählt
nicht; `set ps` **re-armt** den Wächter (der Nutzer darf es erneut versuchen).

**Präzisierung (2026-11-08):** Der Wächter zählt nur, wenn überhaupt ein Hebel
scharf ist (`ps != off` **oder** `radio-save` an) und nur auf dem **Empfänger**.
So löst ein umgebungsbedingter Flap bei `ps off` + `radio-save off` keinen
Falsch-Abbau aus.

### 3. Der sichere Modem-Light-Sleep je Betriebsart

| Betriebsart | Light Sleep | Modem-Sleep | `radio-save` | Ergebnis |
|---|---|---|---|---|
| C6, **assoziiert** (Regelfall) | **aus** (USB) | `ps off` (oder `min` mit Wächter) | **off** | stabil, ~0,12–0,15 W (DTIM) |
| C6, **unassoziiert** (nur ESP-NOW) | aus | `off` | **on** 5000/70 | stabil, ~70 mW |
| klassischer ESP32-Empfänger | aus (Default) | `off` | off | unverändert |

Kernaussage: **Ein „für alle Fälle" sicherer Modem-Light-Sleep ist
`radio-save on` (5000/70) im unassoziierten Betrieb** — er ist am realen Gerät
stabil gemessen (~70 mW, Bursts alle 5 s) und kollidiert mit keinem Beacon.
Im assoziierten Betrieb bringt Modem-Sleep ohnehin kaum etwas (die DTIM-Beacons
dominieren) und ist der link-riskante Fall; dort bleibt `ps off` der Default,
und wer `ps min/max` wagt, wird vom Wächter aufgefangen.

## Behobene Fehler (in diesem Commit)

1. **Compile-Bug (schwer):** `ps_fallback_worker` (beide Rollen) rief
   `diag_log_event()` — sender-only (`#if CONFIG_ESPNOW_ROLE_SENDER`). Alle
   Empfänger-Builds (inkl. **C6**) scheiterten mit
   `error: 'diag_log_event' used but never defined [-Werror]`. → Aufruf in
   `#if CONFIG_ESPNOW_ROLE_SENDER` gekapselt (der Receiver meldet über
   `ESP_LOGW` + `runtime_config_health_note`).
2. **Falsch-Positiv:** Wächter zählte auch ohne aktiven Hebel. → zählt nur bei
   scharfem Hebel und nur auf dem Empfänger.
3. **Test war immer grün:** die `CHECK`-Makro in `test/test_ps_fallback.c`
   erhöhte `g_failures` nie → Suite konnte nie fehlschlagen. → gefixt.

## Verifikation

- Lokaler IDF-v6.1-Build (Docker, Laptop) **aller** Rollen gruen:
  `sender`, `receiver`, `receiver-c6-apoff` (esp32c6). Der Bug-Repro ohne Fix
  belegte `error: 'diag_log_event' used but never defined`.
- Host-Suite `test/run_host_tests.sh` exit 0 (`ps fallback: 17 checks, 0
  failures`).
- CI (GitHub Actions) baut alle fünf Matrix-Rollen und läuft die Host-Tests.
