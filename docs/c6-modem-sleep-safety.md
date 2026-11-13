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
Disconnects in einem gleitenden Fenster. Ein **anhaltender** Sturm (Default:
**20 in 60 s**, über **3 Fenster in Folge**, nach 30 s Boot-Schonfrist) heißt:
der Modem-Sleep bricht den Link. Ein einzelnes über der Schwelle liegendes
Fenster ist nur ein *Kandidat*; erst wenn das nächste Fenster wieder über der
Schwelle liegt (der Sturm also *sustained*), löst der Wächter aus. Dann fällt der
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

## Nachtrag 2026-11-13: der `apoff`-Build kam nie hoch (Fix) + gemessene 0,068 W

**Symptom am Prüfstand (C6FH4, USB):** Der station-only-Build (`role
receiver-c6-apoff`, `CONFIG_ESPNOW_RECEIVER_DISABLE_AP=y`) bootete in einer
Schleife:

```
Guru Meditation Error: Core 0 panic'ed (Stack protection fault)
Detected in task "sys_evt" at 0x40800400
... unmittelbar nach "mDNS ready" + "SNTP time synchronization started"
```

**Ursache:** Im APSTA-Build startet mDNS schon am `WIFI_EVENT_AP_START`, der
GOT_IP-Handler bleibt klein. Mit abgeschaltetem SoftAP gibt es kein AP_START:
`start_mdns()`, die SNTP-Init und `mqtt_bridge_start()` laufen alle im **einen**
`IP_EVENT_STA_GOT_IP`-Handler auf dem Default-Event-Task, den IDF mit **2304
Byte** dimensioniert — zu wenig → Overflow. Genau deshalb kam die *einzige*
sparsame Betriebsart nie hoch.

**Fix:** `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096` in
`sdkconfig.defaults.esp32c6`. Danach bootet `receiver-c6-apoff` sauber (kein
Panic, IP 192.168.178.105, Kanal 11). CI-Lauf `43482262429` grün.

**Messung am Prüfstand (FNB-48, 5,19 V):**

| Konfiguration (`receiver-c6-apoff`) | Leistung | HTTP |
|---|---|---|
| `ps off` + `radio-save on` 20/15 (APSTA-Baseline) | 0,399 W | 200 |
| `ps min` + `radio-save on` 20/15 | 0,397 W | 200 |
| **`ps min` + `radio-save on` 5000/30** | **0,068–0,070 W** | 200 (18/18) |
| `ps min` allein (radio-save off) | 0,068 W | 200 |

**Fazit:** Station-only (kein SoftAP) ist der eigentliche Hebel: erst dort kann
`esp_wifi_set_ps` die STA parken, und der Verbrauch fällt von **0,40 W auf
0,068 W (−83 %)**. Im APSTA-Build ist das strukturell unmöglich, weil die SoftAP
ihre Beacons dauerhaft senden muss (`ESPNOW_IF == WIFI_IF_AP`, `main/main.c`).

**Vorbehalt (offen):** Die FRITZ!Box (Mesh) flappt am Prüfstand auch **ohne**
jeden Hebel (`reason 34` MISSING_ACKS alle ~3 s); das ist die dokumentierte
Umgebungsstörung, nicht der Sleep-Modus. Der `ps_fallback`-Wächter (20/60 s ×3)
schaltet `ps` dann sticky ab, was den Sparmodus wieder aufhebt. Für einen
belastbaren Dauerbetrieb ist der WLAN-Empfang/AP zu stabilisieren (RSSI am
Prüfstand −41…−47 dBm, also gut — die Ursache liegt eher beim AP/DFS).

## Nachtrag 2026-11-07: C6-Empfänger Stromsparmodus — Messwerte + FRITZ-Mesh-Caveat

Am **apoff-C6-Empfänger** (`192.168.178.105`, `v0.4.0-34-gc018e52`,
`/dev/ttyACM0`) mit dem **FNB48**-USB-Powermeter (`tools/fnb48_read.py`) neu
gemessen. Die Konfiguration ist `ps min` **oder** `ps max` + `listen_interval 10`
**ohne** `radio-save`:

| Konfiguration | Leistung |
|---|---|
| `ps off` (Radio dauerhaft wach) | **0,394 W** |
| `ps min`/`ps max` + `listen 10`, `radio-save off` (Ziel) | Median **0,068–0,081 W**, Minimum **0,065 W**, Mittel ~0,10–0,12 W |
| `radio-save on` | Mittel **0,17 W** — schlechter, nicht verwenden |

- Der **Median** ist die ehrliche Ruhelast; der **Mittelwert** liegt höher, weil
  periodische `listen`-Wakes/Bursts ihn hochziehen.
- **`listen_interval 10` ist optimal** — 5/20/30 sind schlechter (mehr
  Reconnect-Bursts). `radio-save on` ist hier **kontraproduktiv** (0,17 W statt
  0,068 W), anders als bei früheren Aufbauten.

**Warum der Sparmodus trotzdem „wiederkam":** Der alte Wächter
`main/ps_fallback.c` schaltete bei **≥8 STA-Disconnects in 300 s** sticky auf
`ps off`. Diese
FRITZ!Box-5530-Mesh-Umgebung erzeugt aber **intermittierende `reason 34`
(MISSING_ACKS)-Disconnects unabhängig vom PS-Modus** — A/B gemessen: `ps off`
**9/60 s** vs `ps min` **11/60 s**. Der Wächter deutet das als Sturm, löst
**Fehlalarm** aus und hebt den Sparmodus auf (**0,068 W → 0,40 W**), obwohl der
PS-Modus unschuldig ist; der Link selbst ist in ruhigen Phasen über 120–180 s
**disconnect-frei**. Lehre: ein reiner Disconnect-Zähler ist als PS-Detektor
untauglich, solange die Umgebung selbst flappt.

**FNB48-Protokoll:** Es gibt **kein USB-Kommando zum Abschalten des internen
Loggers** (Opcodes nur `0x81–0x8A`; `0x84` = Stream stoppen). Framing USB-HID
`AA <cmd> 61×00 <CRC8 poly 0x39 init 0x42>`; Referenz `fnb48s-cli`
(`b5777b1`, Ziel `2e3c:0049`) ist identisch zu `0483:003a`. Bricht man einen Lauf
**ohne `0x84`** ab, läuft der Geräte-FIFO voll und die Anzeige **friert ein**
(der beobachtete „Hänger"). **TWT** wird von der FRITZ!Box 5530 **abgelehnt**
(`TWT Responder:0`) und ist damit unbrauchbar.

**Vorbehalt (Messmethodik):** Belastbar sind die Zahlen nur, wenn der FNB48 **in
Reihe** in der C6-VBUS-Leitung liegt (nicht als Geschwister am Hub) und jeder Lauf
mit `0x84` beendet wird — sonst Standbild/Ripple (siehe
`docs/radio-sleep-break-even.md` § 8.3).
