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

**Präzisierung (2026-11-08):** Der Wächter zählt nur, wenn der Modem-PS-Modus
aktiv ist (`ps != off`) und nur auf dem **Empfänger**. Der ESP-NOW-Wake-Window
allein löst ihn nicht aus (er lässt keine Assoziation fallen, und der gemessene
verlustfreie C6-Default ist genau `ps off` + `radio-save on`). So löst ein
umgebungsbedingter Flap im verlustfreien Default keinen Falsch-Abbau aus.

### 3. Der sichere Modem-Light-Sleep je Betriebsart

Am realen Gerät (.105, C6-Empfänger, klassischer Sender, ~60 Tele./min) gemessen
(s. Messauftrag in `TODO.md`): der **ESP-NOW-Wake-Window (`radio-save`) ist der
eigentliche Hebel**; `ps min/max` **ohne** `radio-save` verliert ~90 % der
Telegramme. Der gemessene Sweet Spot ist ein **kurzes Fenster 20/15 ms**.

| Betriebsart | Light Sleep | Modem-Sleep | `radio-save` | Ergebnis |
|---|---|---|---|---|
| C6, **assoziiert** (Regelfall) | **aus** (USB) | `ps off` | **on** 20/15 | **0,8 % ≈ Baseline (verlustfrei)** |
| C6, wenn Modem-Sleep zwingend | aus | `ps max` + `listen 10` | on 20/15 | ≈ Baseline (verschränkt gemessen) |
| C6, **unassoziiert** (nur ESP-NOW) | aus | `off` | on 20/15 | stabil |
| klassischer ESP32-Empfänger | aus (Default) | `off` | off | unverändert (Wake-Window bricht dort den Link) |

**Nie** `ps min/max` **ohne** `radio-save on` — das ist der 90-%-Fehler. Genau
diesen Default setzt der Messauftrag/Fix (`f12a0e7`) ziel- und rollenabhängig
(`#if defined(CONFIG_IDF_TARGET_ESP32C6) && !CONFIG_ESPNOW_ROLE_SENDER`). Der
`ps_fallback`-Wächter ist das **Sicherheitsnetz**, falls doch ein PS-Modus
erzwungen wird: er greift **nur** bei aktivem `ps != off` (der Wake-Window allein
zerstört die Assoziation nicht) und fällt sticky auf `ps off` zurück.

## Behobene Fehler (in diesem Commit)

1. **Compile-Bug (schwer):** `ps_fallback_worker` (beide Rollen) rief
   `diag_log_event()` — sender-only (`#if CONFIG_ESPNOW_ROLE_SENDER`). Alle
   Empfänger-Builds (inkl. **C6**) scheiterten mit
   `error: 'diag_log_event' used but never defined [-Werror]`. → Aufruf in
   `#if CONFIG_ESPNOW_ROLE_SENDER` gekapselt (der Receiver meldet über
   `ESP_LOGW` + `runtime_config_health_note`).
2. **Falsch-Positiv:** Wächter zählte auch ohne aktiven Modem-PS-Modus. → zählt
   nur bei `ps != off` (der Wake-Window allein zählt nicht) und nur auf dem
   Empfänger.
3. **Test war immer grün:** die `CHECK`-Makro in `test/test_ps_fallback.c`
   erhöhte `g_failures` nie → Suite konnte nie fehlschlagen. → gefixt.

## Merge mit `feat/c6-twt`

Der Wächter steht auf `feat/nvs-web-knobs` (PR #67). Dessen Basis `feat/c6-twt`
enthielt den **gemessenen** Fix (`f12a0e7`) für denselben Themenkomplex
(Default auf die verlustfreie Kombi). Der Merge wurde aufgelöst:
`runtime_config.c` automatisch (beide Seiten behalten), `TODO.md` und
`test/run_host_tests.sh` manuell (beide Testblöcke bzw. Abschnitte behalten).
Der Wächter ist komplementär zum gemessenen Default — er greift nur, wenn ein
PS-Modus erzwungen wird.

## Verifikation

- Lokaler IDF-v6.1-Build (Docker, Laptop) **aller** Rollen gruen:
  `sender`, `receiver`, `receiver-c6-apoff` (esp32c6). Der Bug-Repro ohne Fix
  belegte `error: 'diag_log_event' used but never defined`.
- Host-Suite `test/run_host_tests.sh` exit 0 (`ps fallback: 17 checks, 0
  failures`).
- CI (GitHub Actions) baut alle fünf Matrix-Rollen und läuft die Host-Tests.
