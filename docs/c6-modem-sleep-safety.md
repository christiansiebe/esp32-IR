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

## Nachtrag 2026-11-13: der `apoff`-Build kam nie hoch (Fix) + gemessene Leistung

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
| **`ps min` + `radio-save on` 5000/30** | **0,068–0,070 W** (⚠ s. Korrektur) | 200 (18/18) |
| `ps min` allein (radio-save off) | 0,068 W (⚠ s. Korrektur) | 200 |

> ⚠ **Korrektur 2026-11-14:** Am selben Gerät (jetzt `v0.4.0-35-ga89a416`) ließen
> sich die **0,068 W nicht reproduzieren** — `ps min` + `radio-save on 5000/30`
> ergab erneut **0,146 W** (Median), identisch zu `ps min` allein. Der belastbare
> A/B-Wert ist `ps off` 0,39 W → `ps min` 0,146 W (−62 %). Die 0,068 W bleiben als
> damaliger Messwert stehen, gelten aber als **nicht bestätigt** (vermutlich
> Messartefakt; siehe Nachtrag 2026-11-07).

**Fazit:** Station-only (kein SoftAP) ist der strukturelle Hebel: erst dort kann
`esp_wifi_set_ps` die STA parken. Reproduziert (2026-11-14) fällt der Verbrauch
von **0,39 W auf 0,146 W (−62 %)**. Die früher notierten 0,068 W (−83 %) sind
**nicht bestätigt** (s. Korrektur oben). Im APSTA-Build ist das Parken der STA
unwirksam, weil die SoftAP ihre Beacons dauerhaft senden muss
(`ESPNOW_IF == WIFI_IF_AP`, `main/main.c`).

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
| `ps off` (Radio dauerhaft wach) | **0,39 W** |
| `ps min`/`ps max` + `listen 10`, `radio-save off` (Ziel) | Median **0,146 W**, Minimum **0,129 W**, Mittel 0,15 W |
| `ps min` + `radio-save on 5000/30` | Median **0,146 W** — kein messbarer Vorteil |

- **A/B (2026-11-14, reproduziert):** `ps off` **0,390 W** → `ps min listen 10`
  **0,146 W** = **−62 %**. Das ist der belastbare Spar-Effekt.
- **Korrektur:** Die früher notierten **0,068 W** ließen sich am 2026-11-14 mit
  *keiner* Konfiguration reproduzieren (`ps min` allein, `radio-save on 5000/30`
  und `ps max` ergaben alle ≈0,146 W). Vermutlich war die frühere Zahl ein
  Mess-/Aufbauartefakt (der C6 hängt als Pass-through am FNB-48, `0483:003a`;
  `lsusb` zeigt ihn zusätzlich als `303a:1001`). Bis zu einer
  Reihenschaltungs-Messung gelten die **0,146 W** als aktueller Stand; die
  USB-Serial/JTAG-Peripherie selbst zieht bereits einen relevanten Anteil.
- **`listen_interval 10` ist optimal** — 5/20/30 sind schlechter (mehr
  Reconnect-Bursts). `radio-save on` bringt hier **nichts** (0,146 W statt
  0,146 W), anders als in älteren Notizen.

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

## Externe Referenzen (Espressif/ESPHome) — recherchiert 2026-11-14

> **Kennzeichnung:** Dieser Abschnitt ist **recherchiert** (curl gegen die unten
> genannten URLs, Abruf 2026-11-14), **nicht** am Gerät gemessen. Projektstand
> dabei: Branch `feat/c6-mqtt-cmd`, HEAD `a89a416` (`v0.4.0-35-ga89a416`),
> **ESP-IDF v6.1.0** (`dependencies.lock`, Docker-Image `espressif/idf:v6.1`).
> Frage: Sollte der C6-Empfänger-Stromsparmodus eine **erprobte
> Espressif-Lösung nachbauen** statt eigener Mechanismen?
> **Kurzantwort: nein** — unsere drei Hebel (`esp_wifi_set_ps` /
> `esp_now_set_wake_window` + `connectionless_module_set_wake_interval` /
> `burst-lock`) sind genau die dokumentierten Espressif-Bausteine; die eigenen
> Zusätze (`ps_fallback`, `link_fallback`, `burst_lock`) füllen nur Lücken, die
> Espressif offen lässt (kein eingebauter Disconnect-Sturm-Wächter, keine
> Burst-Vorhersage).

### Quellen und Abgleich

| Quelle | Kernaussage | Bezug zu unserem Aufbau | Handlungsbedarf |
|---|---|---|---|
| **ESP-IDF v6.1 „Wi-Fi Performance and Power Save"** — <https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/api-guides/wifi-driver/wifi-performance-and-power-save.html> | Modem-Sleep nur station-only, stoppt beim Disconnect. `WIFI_PS_MIN_MODEM`: wacht **jedes DTIM** auf, Broadcast geht **nicht** verloren, spart aber wenig, weil DTIM AP-bestimmt ist. `WIFI_PS_MAX_MODEM`: wacht nur alle `listen_interval` auf, **kann Beacons/Broadcast verpassen** (Verlustrisiko steigt mit `listen`). Default ist `WIFI_PS_MIN_MODEM`. | Deckt sich exakt mit unseren Messwerten: `ps min` = Median **0,068 W**, in ruhigen Phasen **0 Disconnects**; `ps max` + `listen`>10 riskiert Verlust (unsere 90-%-Fehlkombi war `ps min/max` **ohne** `radio-save`). `listen_interval` wirkt nur unter MAX. | nein (bereits korrekt umgesetzt) |
| **ESP-IDF v6.1 „Connectionless Modules Power-saving"** (gleiche Seite) | ESP-NOW-RX im Schlaf braucht **Window + Interval**; „At connected state, the start of Interval would be aligned with TBTT"; das effektive Fenster ist das **Maximum** über alle connectionless-Module. | Erklärt die Drift: das driver-geplante Fenster (TBTT-aligned) kann nicht auf den 5-s-Sender-Burst gezielt werden → unser `burst_lock` sagt den Burst stattdessen voraus (siehe `main/burst_lock.h`). | nein (`burst_lock` ist die passende Antwort) |
| **ESP-IDF v6.1 ESP-NOW API** `esp_now_set_wake_window()` Attention 1 — <https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/api-reference/network/esp_now.html> | „This configuration could work at connected status. **When ESP_WIFI_STA_DISCONNECTED_PM_ENABLE is enabled, this configuration could work at disconnected status.**" Default des Wake-Window ist **Maximum** (RX immer an). | Genau der Grund für `CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y` in `sdkconfig.defaults:58–66` (sonst greift das Wake-Window nur assoziiert). | nein — ist schon gesetzt |
| **ESP-IDF v6.1 `components/esp_wifi/Kconfig`** — `ESP_WIFI_STA_DISCONNECTED_PM_ENABLE` (<https://github.com/espressif/esp-idf/blob/v6.1/components/esp_wifi/Kconfig>) | **Default `y`**; „Chip will do modem-sleep when rf module is not in use any more" (gilt **nur im nicht-assoziierten** Zustand). | Bei uns zusätzlich **explizit** =`y`. | nein |
| **ESP-FAQ „Wi-Fi"** (PS-Modi + schwaches Netz) — <https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/wifi.html> | PS-Modi: MIN = `DTIM × 102,4 ms`, MAX = `listen_interval × 102,4 ms`. Für **schwache/gestörte** Netze: Modem-Sleep **nach** `esp_wifi_start()` per `esp_wifi_set_ps(WIFI_PS_NONE)` abschalten und erst nach `IP_EVENT_STA_GOT_IP` aktivieren; **beim Reconnect vorher aus, danach an**. | Espressifs **eigene** Empfehlung für genau unsere Umgebung. Wir setzen `apply_wifi_ps_config()` aktuell auch schon **vor** der Assoziation (PS wird über den Reconnect hinweg gehalten). | **prüfen** (mittlere Prio) |
| **ESPHome Issue #17238** (geschlossen 2026-06-27) + Fix-PR **#17240** (merged 2026-06-27, Merge-Commit `bda7890`) — <https://github.com/esphome/esphome/issues/17238> | Ein hartkodiertes **50 ms Window / 100 ms Interval** (= 50 % RX-Duty) ließ „~half of frames" verloren gehen; das war ein **versehentliches** Throttling bei vorhandenem `deep_sleep:`-Block. Der Fix entfernt das Throttling **ganz** (RX immer wach, PS opt-in). | Unsere **20/15** = **75 % Duty** (15 ms Fenster / 20 ms Intervall) ist **weniger aggressiv** als das als schädlich belegte 50/100. Und: bei uns ist `ps off` + `radio-save 20/15` am Gerät mit **0,8 % ≈ Baseline** gemessen (verlustfrei); `15/12` brachte nichts mehr, `100/70` kostete ~⅓. | nein — 20/15 ist plausibel (siehe (b)) |
| **Espressif Advisory AR2024-006** (Issue-Date 2024/08/30, V1.0) — <https://www.espressif.com/sites/default/files/advisory_downloads/AR2024-006_EN.pdf> | Betrifft **Auto Light Sleep** auf ESP32-C6 (rev ≥ v0.1): **Issue 1** WLAN+BLE-Coexistenz → Beacon-Timeouts/„Wi-Fi communication exceptions"; **Issue 2** WLAN init **ohne** start + Auto-Light-Sleep → `TG0_WDT_HPSYS`-Reset. Gefixt in v5.1.x/v5.2.3/v5.3 (Commits: v5.3 `1aaae85a`+`c56b21bb` bzw. `5feffad9`). | Wir fahren **IDF v6.1.0** (deutlich **nach** allen Fixed Commits) und Light Sleep ist auf dem C6-Build **hart aus** (`runtime_config_light_sleep_allowed()==false`, `ESP_PM_NO_LIGHT_SLEEP`-Lock). **Nicht betroffen.** | nein (nur Doku) |

**Erreichbarkeit der Quellen (2026-11-14):** Alle vorstehenden URLs waren per
`curl` mit **HTTP 200** erreichbar. Zwei Einschränkungen explizit vermerkt:
1. Die **öffentliche Espressif-Advisory-Liste**
   (<https://www.espressif.com/en/support/documents/advisories>) listet für 2024
   nur AR2024-002/005/009 — **AR2024-006 ist dort nicht verlinkt**; das PDF war
   nur über die **Direkt-URL** `.../advisory_downloads/AR2024-006_EN.pdf`
   abrufbar (kein 404, 75 KB, 3 Seiten, verifiziert).
2. Eine **eigene „ESP-NOW FAQ"-Seite** existiert nicht; die ESP-NOW-Aussagen
   stehen in der **API-Referenz** (`esp_now.html`) und im Abschnitt
   „Connectionless Modules Power-saving" der Wi-Fi-Power-Save-Seite, ergänzt um
   die **ESP-FAQ**. Alle drei wurden ausgewertet.

### Antworten auf die vier Fragen

**(a) Sollten wir `CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE` setzen?**
Er ist **bereits gesetzt** (`sdkconfig.defaults:66`, zusätzlich in allen
`_sdkconfig`-Artefakten) und in IDF v6.1 ohnehin **Default `y`**. Er ist die
Voraussetzung dafür, dass `esp_now_set_wake_window()` auch **unassoziiert**
wirkt (API-Attention 1) — genau der Punkt, der den früheren Wake-Window-Versuch
scheitern ließ. **Keine Änderung nötig; nicht abschalten.**

**(b) Ist unsere 20/15-ms-Wahl gegen Quelle 3 plausibel oder zu aggressiv?**
**Plausibel, nicht zu aggressiv.** Unsere 20/15 (Interval 20 ms, Window 15 ms) =
**75 % RX-Duty**; das von ESPHome als schädlich belegte Beispiel lag bei
**50 %** (50/100). Wir sind also *konservativer* als der dokumentierte
Fehlerfall. Zusätzlich stützt die eigene Messung die Wahl: `ps off` + `radio-save
20/15` → **0,8 % ≈ Baseline** (verlustfrei), `15/12` bringt nichts mehr
(kleiner geht nicht sinnvoll), `100/70` kostet ~⅓. Wichtige Einschränkung: 20/15
ist **nur zusammen mit dem receiver-Default `ps off` + `burst-lock`** der
Sweet Spot; als *alleinigen* universellen Default (ohne Burst-Vorhersage) würde
ich es **nicht** festschreiben — genau davor warnt Quelle 3. Der aktuelle
C6-Receiver-Default (`radio_save=false`, `burst_lock=true`, 150-ms-Fenster) ist
die verlustfreie UND sparsame Kombi und bleibt richtig.

**(c) Gibt es eine Espressif-Empfehlung gegen `reason 34` bei PS?**
**Keine spezifische** „reason-34"-Doku. `WIFI_REASON_MISSING_ACKS = 34` ist in
`esp_wifi_types_generic.h` (v6.1) nur als „Missing ACKs" definiert. Ableitbar
aus den Quellen ist:
1. **MIN statt MAX** verwenden (MIN wacht jedes DTIM, verliert keinen
   Broadcast) — deckt sich mit unserer Beobachtung, dass MAX nur mit
   `radio-save`/`listen` klein sicher ist.
2. **PS erst nach `IP_EVENT_STA_GOT_IP`** aktivieren und **beim Reconnect
   vorher abschalten** (ESP-FAQ-Empfehlung für schwache/gestörte Netze).
3. Wenn es trotzdem flappt: **`WIFI_PS_NONE`** — exakt unser
   `ps_fallback`-sticky-Rückfall.
Da `reason 34` hier **auch ohne** jeden Hebel auftritt (Umgebungsstörung der
FRITZ!Box-5530-Mesh), liegt die belastbare Abhilfe **AP-seitig** (Mesh/Band-
Steering/DFS/Kanal), nicht im Sleep-Modus. Unser `ps_fallback` (nur bei
aktivem `ps != off`, anhaltender Sturm 20/60 s ×3) ist damit
Espressif-konform: er greift genau dann, wenn die drei Empfehlungen oben
nicht mehr helfen.

**(d) Was davon gehört in einen GitHub-Issue/PR?** (Empfehlung — **nichts
erstellt**, kein Push)
- **Kein** PR im eigenen Repo nötig: alle vier Quellen bestätigen den
  **Ist-Zustand** (Kconfig gesetzt, MIN-Default, 20/15 plausibel, Light Sleep
  aus, AR2024-006 nicht betroffen). Ein Firmware-Change wäre durch die Quellen
  **nicht** gedeckt.
- **Optionaler Kandidat** für einen eigenen **Issue** (nicht Code-PR): die
  **reason-34-Flapping der FRITZ!Box 5530 Mesh unabhängig vom PS-Modus** mit
  Messwerten (`ps off` 9/60 s vs `ps min` 11/60 s; A/B-verschränkt) + Bitte um
  AP-seitige Klärung (Mesh/DFS/Band-Steering). Das ist eine
  Umgebungs-/Router-Frage, kein Firmware-Bug.
- **Nicht** sinnvoll: ein Upstream-Issue bei espressif/esp-idf — es gibt keine
  offene Verhaltenslücke, die unsere Messwerte belegen (AR2024-006 ist in v6.1
  gefixt, die PS-Semantik ist dokumentiert).

**Vorbehalt (Messmethodik):** Belastbar sind die Zahlen nur, wenn der FNB48 **in
Reihe** in der C6-VBUS-Leitung liegt (nicht als Geschwister am Hub) und jeder Lauf
mit `0x84` beendet wird — sonst Standbild/Ripple (siehe
`docs/radio-sleep-break-even.md` § 8.3).

## Nachtrag 2026-11-14: `ps_fallback` auf „sustained storm" umgestellt + validiert

**Ursache des wiederkehrenden Sparmodus-Verlusts:** Der alte Wächter latschte
`ps off` schon bei **8 Disconnects in 300 s**. Die FRITZ!Box-5530-Mesh-Umgebung
erzeugt `reason 34` (MISSING_ACKS) aber **unabhängig vom PS-Modus** (A/B: `ps off`
9/60 s vs `ps min` 11/60 s), also löste ein reiner Zähler **Fehlalarm** aus und
verwarf den Sparmodus (0,39 W statt 0,146 W).

**Fix (`a89a416`):** Der Wächter verlangt jetzt einen **anhaltenden** Sturm —
ein 60-s-Fenster über der Schwelle (Default **20**, ~12× der Umgebungs-Spitze)
ist nur ein *Kandidat*; erst **3 über der Schwelle liegende Fenster in Folge**
laschen den Fehler. Ein isolierter Umgebungs-Burst ist im nächsten Fenster vorbei
und „sustained" nie; ein echter Modem-Sleep-Schaden hält den Link unten und ist
in ~3 min bestätigt. `CONFIG_ESPNOW_PS_FALLBACK_SUSTAIN_WINDOWS=1` stellt das alte
Verhalten wieder her. Kconfig: Fenster 300 s→60 s, Schwelle 8→20, neuer
Sustain-Knopf; Host-Test deckt Sustain, Reset durch ruhiges Fenster, Schonfrist,
intentional-Reconnect und Fenster-Gleiten ab.

**CI-Verifikation (GitHub, `a89a416`):** Host-Tests (aggregation) **grün** +
alle fünf Matrix-Rollen (`receiver`, `receiver-apoff`, `receiver-c6`,
`receiver-c6-apoff`, `sender`) **grün**. Lauf `43526156557`.

**Realwelt-Validierung am Gerät** (`v0.4.0-35-ga89a416`, apoff-C6):
- `set ps min` → **hält stabil über den Beobachtungszeitraum** (`ps_cfg=min`,
  `reconn=0`; der Wächter latscht trotz Umgebungsrauschen **nicht** mehr).
- A/B-Leistung: `ps off` **0,390 W** → `ps min listen 10` **0,146 W** (−62 %).

**Externe Referenzen (recherchiert, 2026-11-14):**
- `CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE` ist bereits gesetzt
  (`sdkconfig.defaults`) und IDF-v6.1-Default `y`; er macht
  `esp_now_set_wake_window()` auch **unassoziiert** wirksam. Beibehalten.
- Unsere `radio-save` 20/15 (75 % RX-Duty) ist **konservativer** als der
  ESPHome-Issue-#17238-Fehlerfall (50 %, 50/100) — plausibel, nicht zu aggressiv.
- Keine Espressif-Doku speziell zu `reason 34`; die Quellen stützen MIN statt
  MAX, PS erst nach `GOT_IP`, sonst `WIFI_PS_NONE` — genau unser sticky
  `ps_fallback`. Die reason-34-Flapping tritt **auch mit `ps off`** auf, ist also
  eine AP-/Mesh-Umgebungsfrage (Mesh/DFS/Band-Steering), kein Firmware-Bug.
- Espressif-Advisory **AR2024-006** (C6 Auto-Light-Sleep) ist in v6.1 gefixt und
  betrifft uns nicht (Light Sleep am USB-C6 ohnehin aus).

## Nachtrag 2026-11-14 (2): der eigentliche Stromfresser — `burst-lock` ohne Sender

**Ursache „alle Versuche scheiterten":** Der C6-Receiver-Default ist
`ps min` **+ `burst_lock=true`** (`main/runtime_config.c`). Der
`burst_lock_worker` hält das Radio **absichtlich dauerhaft wach, solange der
Burst noch nicht gelernt ist** (`if (!locked) hold awake`), damit der erste
Burst nicht verpasst wird. **Ohne Sender in Reichweite** (`sender_rssi_valid
= false`, unser Prüfstand) lernt er nie → das Radio bleibt **dauerhaft wach** und
negiert `ps min` komplett.

**A/B am Gerät (apoff-C6, `ps min`, `listen 10`):**

| Konfiguration | Leistung (Median) |
|---|---|
| `burst-lock on` (ungelernt, kein Sender) | **0,392 W** |
| `burst-lock off` | **0,147 W** |

**Fix (`02cf313`):** Der Lern-Hold ist jetzt auf **60 s** begrenzt
(`BURST_LOCK_LEARN_TIMEOUT_MS`). Ein *anwesender* Sender sendet binnen Sekunden
→ sein erster Lock ist unberührt; **ohne** Sender parkt der Worker das Radio nach
dem Timeout und beobachtet nur noch (das nächste, ggf. auf einem DTIM-Wake
eintreffende Frame lernt den Lock neu). Der ESP-NOW-Exchange-Hold (OTA/
getunnelter Befehl) bleibt unverändert.

**Verifikation:** CI `43530964926` (Host-Tests + alle fünf Rollen) **grün**;
geflasht als `v0.4.0-38-g02cf313`. Messung danach, `burst-lock on` + `ps min`
**ohne Sender, nach Timeout**: Median **0,135 W** (vorher 0,392 W) — der Fix
greift. Damit ist der Receiver-Default nun auch ohne Sender sparsam.

**Einordnung der früheren 0,068 W:** Sie gelten für den **gelockten** Pfad
(Sender anwesend; der Worker parkt zwischen den Bursts). Der **ungelernte** Pfad
war der Stromfresser und ist jetzt ebenfalls geparkt.

## Nachtrag 2026-11-14 (3): ehrliche Leistungsmessung + `ps min` vs `ps max` + FNB-48-Freeze

**Messmethodik-Fehler korrigiert.** Frühere Notizen nannten „Median 0,068 W" für
`ps min`. Das ist der **geparkte Floor** (min), nicht der Mittelwert. Das Gerät
parkt (≈0,065 W) und **wacht periodisch auf** (≈0,32–0,38 W), weil das
Wi-Fi-Modem im PS-Modus alle paar hundert ms kurz aufwacht. Der ehrliche
**Mittelwert** liegt höher. Ab jetzt werden avg/med/min + Parkanteil getrennt
berichtet.

**A/B am apoff-C6 (`v0.4.0-38-g02cf313`, `burst-lock off`, kein Sender):**

| Konfiguration | Ø | median | min | geparkt (<0,09 W) |
|---|---|---|---|---|
| `ps min` listen 10 | 0,142 W | 0,130 W | 0,065 W | 26–35 % |
| `ps max` listen 10 | 0,130 W | 0,125 W | 0,065 W | 47 % |
| **`ps max` listen 30** | **0,095 W** | 0,067 W | 0,065 W | **79 %** |
| `ps max` + burst-lock on (Lernphase) | 0,388 W | 0,387 W | 0,381 W | 0 % |

- **`ps min` ≠ `ps max`:** Entgegen der früheren Notiz ist **`ps max` sparsamer**
  (parkt länger; MIN wacht jedes DTIM → öfter). Der Receiver-Default bleibt
  dennoch `ps min`, weil er **nur zusammen mit `burst-lock`** (Sender anwesend)
  sinnvoll ist — der Burst-Lock hält das Radio genau um die Bursts wach.
- **`listen 30` hilft** (weniger Reconnect-/Beacon-Wakes), kostet aber
  Reaktionszeit.
- **Mit Sender** (gelockt) gilt weiter der dokumentierte niedrige Wert; ohne
  Sender greift jetzt der Lern-Timeout (siehe (2)).

**Receiver-Default nach dem Fix (kein Sender, nach Lerntimeout):** Ø **0,141 W**
(vorher, ungefedert gepinnt: 0,392 W). Der Fix wirkt.

**FNB-48 „Freeze" behoben (`d459bc4`).** Der Meter friert ein, wenn eine Messung
**ohne das `0x84`-Stop** endet (Ctrl-C, SIGTERM, Exception, gekillter Job) — sein
interner Logger läuft weiter, der Puffer füllt sich, die Live-Werte stehen.
`tools/fnb48_read.py` sendet das Stop jetzt auf **jedem** Pfad (auch bei
SIGINT/SIGTERM, via KeyboardInterrupt im `finally`) und vor jedem Start doppelt.
Verifiziert: nach SIGTERM mitten in einer Messung liefert der nächste Lauf sofort
wieder frische Werte (CRC ok).
