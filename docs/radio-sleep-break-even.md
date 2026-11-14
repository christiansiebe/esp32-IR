# Funk-Modem-Sleep des ESP32-C6-Empfängers — Break-even und maximales sicheres `wake-interval`

Stand: 2026-11-07 · Branch `feat/esp32c6-receiver` · reine Lese-/Rechennotiz
(nichts kompiliert, nichts geflasht, keine Firmware geändert).

Frage: **Wie lange darf der Empfänger das Funkmodem schlafen legen
(`set radio-save on`, `wake-interval`, `wake-window`), bevor echte
Datenverluste entstehen?**

## TL;DR

| Größe | Wert | Beleg |
|---|---|---|
| Telegramm-Rate (SML, Normalfall) | ~1/s = **60/min** | `main/main.c:123`, `main/main.c:135` |
| Sender-Ring (Kapazität) | **256 Telegramme** | `main/tx_batch.h:27` |
| **Max. Zeit ohne Pull (Ring-Limit)** | **256 s ≈ 4 min 16 s** | 256 / 1 |
| Burst pro Sendezyklus | ≤ 32 Tele., real ~5 | `main/main.c:136`, `main/main.c:135` |
| Backfill-Intervall | **30 min** (+10 s bis „caught up") | `main/main.c:487`, `main/main.c:6493` |
| Backfill-Fenster (automatisch) | **3 Tage** | `main/history_backfill.h:26` |
| Minuten-Historie im SPIFFS | **29000 min ≈ 20,1 Tage** | `main/power_history.h:17` |
| **Empfohlen `wake-interval`** | **5000 ms** (sicher, kein Backlog) | s. Abschnitt 5 |
| **Absolutes Limit** | **≤ 250 s** (Ring-Limit mit Reserve) | 256 s − Reserve |
| Empfohlen `wake-window` | **70–100 ms** | `main/Kconfig.projbuild:103`, `main/runtime_config.c:39` |
| Dauer-RX-Leistung `P_rx` | **0,42 W** (gemessen) | `AGENTS.md:181` |

Kernaussage: **Kein Live-Pull bis ~256 s ist unkritisch**, weil der
Sender-Ring 256 Telegramme hält; danach verwirft der Sender die ältesten
(`dropped`, `sml_tx_batch_missing()` wächst). **Echte (dauerhafte)
Datenverluste** entstehen aber erst, wenn die **Minuten-Historie im SPIFFS**
überläuft — das sind **~20 Tage** — und selbst dazwischen füllt der
**Backfill** (alle 30 min, Fenster 3 Tage) jede Live-Lücke nach.

---

## 1. Datenfluss (belegt)

- **Sender** liest den SML-Strom (9600 Baud 8N1, UART1) und dekodiert
  Telegramme (`main/main.c:94-96`).
- Jedes dekodierte Telegramm wird in den **Ring** `g_sml_tx_batch` gelegt
  (`main/tx_batch.h:42-50`).
- Ein Worker weckt alle **5 s** (`SML_TX_CYCLE_MS`, `main/main.c:133`) und
  sendet bis zu **32** Telegramme als **Burst** per ESP-NOW-Broadcast
  (`SML_TX_BURST`, `main/main.c:136`; `flush_tx_batch()`,
  `main/main.c:3354-3377`; `uart_transport_worker()`,
  `main/main.c:3379-3389`).
- Der **Empfänger** speist empfangene Telegramme in seine Minuten-Historie
  (`main/main.c:2626-2733`) und gleicht fehlende Minuten per **Backfill**
  gegen die Sender-SPIFFS ab (`main/history_backfill.c/.h`,
  `main/history_sync.c/.h`).

Wichtig: der Empfänger bestätigt den SML-Broadcast **nicht** selbst; der
Sender wertet den ESP-NOW-Sendecallback als „bestätigt"
(`main/main.c:3073-3074`, `main/main.c:3118-3131`). Bei einem
unbestätigten Send wird das Telegramm vorne in den Ring zurückgelegt und
der Burst abgebrochen (`main/main.c:3370-3375`, `sml_tx_batch_requeue_front`,
`main/tx_batch.c:53-73`) — **das ist der Mechanismus, der den Ring bei
schlafendem Empfänger volllaufen lässt** (siehe Abschnitt 3).

---

## 2. Telegramm-Rate (SML-Standardfall)

Der Code belegt die Rate an zwei Stellen explizit:

- `main/main.c:123`: „A telegram normally arrives about **once per second**."
- `main/main.c:135`: „Normal arrival is **~1/s** so a 5 s window has ~5 to send."

Abgeleitet:

```
Rate   = 1 Telegramm/s
       = 60 Telegramme/min
       = 5 Telegramme pro 5-s-Sendezyklus
```

Zur **Minuten-Historie** wird daraus **1 gemittelter Record pro Minute**
(nicht 1 Record pro Telegramm): der 1-Hz-Sample-Timer
(`esp_timer_start_periodic(..., 1000000)`, `main/main.c:8632`) mittelt die
Sekundenwerte über `POWER_HISTORY_INTERVAL_SECONDS = 60`
(`main/main.c:146`, Bucket-Abschluss `main/main.c:2848-2870`).

---

## 3. Wann läuft der Sender-Ring über? (Kapazität / Rate)

**Kapazität:** `SML_TX_BATCH_CAPACITY = 256u` (`main/tx_batch.h:27`, Kommentar
`main/tx_batch.h:26`: „Room for far more than one 5 s cycle (telegrams arrive
~1/s) before evicting").

**Eviction:** ist der Ring voll, wird das **älteste** Element verworfen und
`dropped` erhöht (`main/tx_batch.c:20-25`). `sml_tx_batch_missing()`
(`main/tx_batch.c:92-101`) zählt die so verlorenen Sequenzen — „0 im
Normalbetrieb; ein wachsender Wert ist das Reconciliation-Signal"
(`main/tx_batch.h:80-83`). Der Wert wird als Diagnose exportiert
(`main/main.c:1893-1894`).

**Rechnung (Kapazität / Rate):**

```
t_overflow = SML_TX_BATCH_CAPACITY / Rate
           = 256 Telegramme / 1 Telegramm/s
           = 256 s
           = 4 min 16 s
```

**Antwort:** Verpasst der Empfänger **mehr als 256 s** ohne Pull, verwirft der
Sender die ältesten Telegramme → echte Live-Lücke (`sml_tx_batch_missing()`
steigt). 256 s ist damit die **maximale Zeit, die der Empfänger ohne Pull
verpassen darf**, ohne dass der Ring überläuft.

### Feinheit A — der Sender gibt höchstens 32 Tele. pro 5-s-Zyklus heraus

Der Ring *entleert* sich nur mit der Burst-Rate: ≤ 32 Telegramme pro 5 s =
**6,4 Tele./s** (`main/main.c:136`, `main/main.c:3361`). Bei einer
Ankunftsrate von 1/s heißt das: **um den Ring dauerhaft konstant zu halten,
muss der Empfänger im Mittel zu ≥ 1/6,4 = 15,6 % der Zeit wach sein**
(Schlaf-Duty-Cycle ≤ 84 %). Ein Empfänger, der pro Aufwachen nur einen
einzigen Burst fängt, entleert pro Aufwachen nur ≤ 32 Telegramme — bei
1/s Ankunft ist das nur nachhaltig, solange der Sendeabstand ≤ 32 s ist.
Praktisch heißt das: `wake-interval` als **kleines Vielfaches des 5-s-Zyklus**
wählen, damit die Fenster mit den Bursts fluchten (z. B. 5 s, 10 s, 30 s).

### Feinheit B — puffert der Ring bei schlafendem Empfänger überhaupt?

Ob der Ring während des Empfänger-Schlafs wirklich vollläuft, hängt davon ab,
ob der Sender den Broadcast als „unbestätigt" erkennt. ESP-NOW-**Broadcast**
wird auf MAC-Ebene nicht bestätigt; `esp_now_send()` meldet dann
`ESP_NOW_SEND_SUCCESS`, sobald das Frame abgestrahlt ist. Trifft das zu,
läuft der Ring **nicht** voll, der Sender sendet „ins Leere", und verpasste
Telegramme erscheinen beim Empfänger nur als **Sequenzlücke**
(`main/main.c:2684-2707`), die der Backfill schließt. Dann greift das
256-s-Limit nicht und es bleibt nur das SPIFFS-Limit (Abschnitt 4).
**Das ist am Bench zu verifizieren** (siehe Abschnitt 7).

---

## 4. Wie viele Minuten passen in die History-SPIFFS?

Beide Rollen führen dieselbe `power_history`-Minutenhistorie im
`storage`-SPIFFS (`AGENTS.md:110-112`).

**Record:** 28 Byte, gepackt (`main/power_history.h:76-85`):
`sequence` 4 + `interval_start_utc` 8 + `average_milliwatts` 4 +
`sample_count` 4 + `format_version` 2 + `reason` 1 + `reserved` 1 +
`checksum` 4 = **28 B** (Literal „28 B" auch in `main/power_history.h:19`).

**Header:** 28 Byte, **zweimal** in der Datei
(`main/power_history.h:100-109`; Offset-Formel
`2u*sizeof(header) + slot*sizeof(record)`, `main/power_history.c:196-198`).

**Partition:** `storage` bei Offset `0x320000`, Größe `0xE0000`
(`partitions.csv:7`) = **917504 B = 896 KB** (`AGENTS.md:98`).

**Rechnung:**

```
Datei     = 2 * 28 B  +  29000 * 28 B
          = 56 B      +  812000 B
          = 812056 B  ≈ 793 KiB
Partition = 917504 B
frei      = 917504 - 812056 = 105448 B ≈ 103 KiB   (SPIFFS-Reserve/GC)
```

Deckt sich exakt mit dem Code-Kommentar „~103 kB of free SPIFFS"
(`main/power_history.c:676`) und `AGENTS.md:119-125`.

**Kapazität:** `POWER_HISTORY_CAPACITY = 29000u` (`main/power_history.h:17`).
Bei 1 Record = 1 Minute:

```
29000 min = 483,3 h = 20,14 Tage        (Kommentar „~20 days", power_history.h:15)
```

Ring-Wrap: `next_slot = (next_slot + 1) % POWER_HISTORY_CAPACITY`
(`main/power_history.c:421-424`) — nach 20 Tagen wird die älteste Minute
überschrieben. Gegenprobe 30000 Records = 840056 B passte **nicht** (zu wenig
Reserve → `SPIFFS_ERR_FULL`, `main/power_history.h:12-14`, `AGENTS.md:121-124`).

**Backfill (Sicherheitsnetz):** Der Empfänger holt verpasste Minuten
periodisch nach — alle **30 min** (`HISTORY_BACKFILL_PERIOD_MS`,
`main/main.c:487`), alle **10 s** solange nicht „caught up"
(`main/main.c:6493`), und sofort bei manuellem Pull (`main/main.c:4533`).
Pro Session max. 64 Seiten × 7 Records = 448 Minuten
(`main/main.c:486`, `HISTORY_SYNC_PAGE_RECORDS 7`, `main/history_sync.h:18`).
Automatisches Fenster **3 Tage** (`main/history_backfill.h:26`).

**Antwort:** In die History passen **29000 Minuten ≈ 20 Tage** pro Rolle. Eine
Live-Lücke wird spätestens 30 min später automatisch nachgezogen, solange die
Minute innerhalb des 3-Tage-Fensters und im Sender-Ring (20 Tage) liegt.
**Permanenter Verlust** tritt erst ein, wenn Sender- **oder** Empfänger-SPIFFS
überläuft, also nach **~20 Tagen** ohne jeden Backfill.

---

## 5. Empfehlung: `wake-interval` und `wake-window`

### 5.1 `wake-window` — muss den Burst treffen

Ein Burst ist **nur wenige ms** lang: ~5 Frames pro 5 s
(`main/main.c:135`), ein Frame ≈ 35–67 Byte Nutzlast
(`sml_transport_header_t` 19 B + 1–3 × `sml_transport_value_t` 16 B,
`components/sml_transport/include/sml_transport.h:9-28`), bei 1 Mbit/s
inkl. Wi-Fi-Overhead ~1–2 ms → **Burst ≈ 5–15 ms**.

Der bewährte Sender-Default und die Kconfig-Empfehlung sind **70 ms Fenster**
(`main/Kconfig.projbuild:103-112`: „~70 % of the interval is a good trade-off";
`main/runtime_config.c:39-40`). Das ist 5–20× der Burst-Dauer — komfortabel.
Ein Fenster **< 20 ms** wäre knapp; **70–100 ms** ist sinnvoll.

### 5.2 `wake-interval` — abgestuft

| Ziel | `wake-interval` | Begründung |
|---|---|---|
| **Sicher (empfohlen, kein Backlog, keine Lücke)** | **5000 ms** | trifft jeden Sendezyklus; Ring wächst nicht |
| Sicher, weniger Aufwachen | 10000 / 15000 / 30000 ms | Vielfache des 5-s-Zyklus, Fenster fluchten mit den Bursts; Backlog (≤ 30 Tele.) entleert ein Burst (≤ 32) in einem Zug |
| **Absolutes Ring-Limit** | **≤ 250 s** | 256 s Kapazität/Limit (Abschnitt 3) minus Reserve |
| Grenze für „kein permanenter Verlust" | **~20 Tage** | SPIFFS-Minutenhistorie (Abschnitt 4), Backfill füllt Lücken |

**Klar gesagt:**

- **Maximal sicheres `wake-interval` (Ring-Limit) = 250 s.** Darüber hinaus
  verwirft der Sender die ältesten Telegramme (`sml_tx_batch_missing()` steigt,
  `main/main.c:1893-1894`) → Live-Lücke.
- **Empfohlen im Betrieb: `wake-interval = 5000 ms`, `wake-window = 70–100 ms`.**
  Nur so fluchten die Fenster mit den Bursts und der Ring bleibt stabil
  (Feinheit A). Wer länger schlafen will, nimmt ein Vielfaches von 5000 ms.
- Auch bei deutlich längerem Schlaf gibt es **keinen permanenten Verlust**,
  solange die 20-Tage-SPIFFS und der 30-min-Backfill greifen — die Live-Lücke
  wird nachgezogen.
- Der Default `100/70` (`main/runtime_config.c:39-40`, Duty 70 %) bringt
  praktisch **keine** Ersparnis; erst ein größeres Intervall wirkt.

---

## 6. Energie-Break-even

Formel (Mittelwert über ein Intervall; `W` = `wake-window`, `I` = `wake-interval`):

```
P(I) = P_sleep + (W / I) * (P_rx - P_sleep)
```

mit

- `P_rx` = **0,42 W** Dauer-RX (gemessener C6-Empfänger, `AGENTS.md:181`;
  Baseline 0,433 W, `AGENTS.md:179`, `README.md:114-116`),
- `P_sleep` = **noch zu messen** (Modem-Sleep-Strom des C6).

Ersparnis gegenüber Dauer-RX (`I → ∞`):

```
ΔP = P_rx - P(I) = (P_rx - P_sleep) * (1 - W / I)
```

Duty-Cycle `W/I` für Kandidaten:

| `wake-window` | `wake-interval` | Duty `W/I` |
|---|---|---|
| 70 ms | 100 ms (Default) | 70,0 % |
| 70 ms | 5000 ms | 1,40 % |
| 70 ms | 30000 ms | 0,23 % |
| 100 ms | 5000 ms | 2,00 % |

Beispiel mit Platzhalter `P_sleep = 0,05 W` (nur zur Veranschaulichung — **nicht
gemessen**):

```
P(5 s)   = 0,05 + (0,070/5)   * (0,42 - 0,05) = 0,0552 W
P(30 s)  = 0,05 + (0,070/30)  * (0,42 - 0,05) = 0,0509 W
ΔP(5 s)  = 0,42 - 0,0552 ≈ 0,365 W   (≈ 87 % weniger als Dauer-RX)
```

Der echte Gewinn hängt vollständig an `P_sleep`: nur wenn das geparkte Modem
deutlich unter `P_rx = 0,42 W` liegt, lohnt der Umbau. **`P_sleep` mit dem
FNB-48-USB-Leistungsmesser messen** (dasselbe Setup wie `AGENTS.md:178`), dann
`P(I)` mit der Formel oben rechnen.

---

## 7. Offene Punkte / am Bench zu klären

1. **`P_sleep` messen** (Abschnitt 6) — der ganze Break-even hängt daran.
2. **Broadcast-Bestätigung prüfen** (Feinheit B, Abschnitt 3): meldet
   `esp_now_send()` an `broadcast_mac` (`main/main.c:3092`) bei schlafendem
   Empfänger `ESP_NOW_SEND_SUCCESS`? Davon hängt ab, ob das 256-s-Ring-Limit
   überhaupt greift oder nur das SPIFFS-Limit (20 Tage).
3. **Burst-Fluchtung prüfen:** Bei `wake-interval` als Vielfaches von 5000 ms
   ist die relative Phase fix — Fenster und Burst müssen einmal ausgerichtet
   sein, sonst wird nie ein Burst getroffen. Ggf. ein Vielfaches **+ Offset**
   oder ein Fenster ≥ 5000 ms wählen.
4. **Backfill über schlafendem Funk:** Da der Backfill selbst ESP-NOW nutzt,
   muss das Modem während der Backfill-Session wach sein (die Session läuft im
   `history_backfill_worker`, `main/main.c:6455`); sicherstellen, dass
   `wake-window` die Backfill-Anfragen nicht verpasst.

---

## 8. USB-Konsole vs. Light-Sleep (C6)

Stand: 2026-11-07 · Branch `feat/esp32c6-receiver` · Doku-Notiz (Befund +
Fix-Beschreibung + Messwerte; kein Code in dieser Datei).

### 8.1 Befund

Beim C6-Receiver-Build schaltet der Laufzeit-Knopf `set sleep on` (automatic
light sleep) die **USB-Serial/JTAG-Peripherie ab**. Die Konsole auf
`/dev/ttyACM0` verschwindet. Weil die Einstellung in **NVS persistiert** wird
(Namespace `rtcfg`, Key `cfg`, Blob-Version 3), bootet das Geraet anschliessend
**im Light Sleep** und der USB-Port **enumiert nie wieder** — das Geraet ist
ueber USB unerreichbar (**Flashen und Konsole tot**).

Ursache: `SOC_USB_SERIAL_JTAG_SUPPORT_LIGHT_SLEEP` ist fuer den C6 **nicht
definiert** (IDF-6395 offen). Die USB-Serial-JTAG-Peripherie ueberlebt Light
Sleep also nicht — sie faellt mit dem Schlafmodus weg und kommt ohne
Re-Enumeration nicht zurueck.

### 8.2 Fix (umgesetzt, CI-gruen, Commit 7e34996)

Auf Builds mit USB-Serial-JTAG-Konsole (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`)
wird Light Sleep **komplett abgelehnt**:

- `set sleep on` wird **verworfen**.
- Ein aus NVS restauriertes `light_sleep=1` wird auf `0` **gezwungen** (clamp).
- `esp_pm_configure()` bekommt **nie** `light_sleep_enable=1`.
- Zusaetzlich haelt der Empfaenger einen **`ESP_PM_NO_LIGHT_SLEEP`-Lock**.
- **Modem-/Radio-Sleep (`radio-save`) bleibt erhalten.**

**Selbstheilung:** Die **NVS-Blob-Version steigt 3 → 4**, damit ein bereits
persistierter kaputter `light_sleep`-Wert beim naechsten Boot verworfen wird.

**Recovery:** Die **BOOT-Taste (3 s)** loescht zusaetzlich den NVS-Namespace
`rtcfg`, damit ein schlechtes persistiertes Config-Set den Rollback nicht
ueberlebt.

### 8.3 Messwerte (FNB-48 USB-Leistungsmesser)

> ⚠️ **KORREKTUR (2026-11-07, nachpruefbar): Die folgende Tabelle ist NICHT
> belastbar und darf nicht als C6-Verbrauch gelesen werden.**
>
> 1. **Das Messskript fror ein.** `fnb48_read.py` sendete nur Start (`0x82`) +
>    Keep-alive (`0x83`), aber **nie Stop (`0x84`)**. Der interne
>    Stream/Logger des FNB-48 lief weiter, sein Puffer lief voll und die
>    Anzeige lieferte **Standbilder** statt Live-Werte. Das Stop-Kommando
>    `STOP_STREAM = 0x84` (CRC `a6`) ist im Referenz-CLI `fnb48s-cli`
>    dokumentiert; `fnb48_read.py` sendet es jetzt vor dem Start (haengenden
>    Stream loeschen) und nach dem Lesen (Geraet freigeben). Danach kamen
>    wieder frische, wechselnde Werte (208 Samples/Lauf, CRC ok).
> 2. **Der FNB-48 liegt nicht im C6-Strompfad.** Topologie (`lsusb -t`): der
>    C6 (`303a:1001`) haengt als **Geschwister** am Hub-Port `1-1.3`, der
>    FNB-48 (`0483:003a`) an `1-1.1` und hat **keine Kinder**. Der C6-Strom
>    fliesst also gar nicht durch den Messgeraet-Pfad.
> 3. **Gegenprobe mit frischen Werten:** Schaltet man den C6 per
>    `uhubctl -l 1-1 -p 3 -a off` ab (lsusb `303a` -> 0, `/dev/ttyACM0`
>    verschwindet, Port `0000 off`), bleibt die Anzeige bei **~0,065 W** —
>    sie folgt dem C6 **nicht**. Der Wert ist der Ruheverbrauch von
>    Messgeraet/Hub.
>
> **Konsequenz:** Die alten Zahlen (0,432 / 0,298 / 0,068 / 0,024 / 0,028 W)
> sind mit dem eingefrorenen Skript entstanden und **nicht** als C6-Verbrauch
> zu verwerten. Fuer eine belastbare Messung muss der FNB-48 **in Reihe** in
> die C6-VBUS-Leitung (USB-C-Messadapter/-Durchschleifer) gesteckt und mit
> dem gepatchten Skript gelesen werden. Die qualitative Aussage (Light Sleep
> killt die USB-Konsole) bleibt aus dem Fix-Code korrekt; die Watt-Zahlen
> gelten bis zu einer sauberen Reihenmessung als **offen**.

| Konfiguration | Leistung | Anmerkung |
|---|---|---|
| klassischer AP-Build (Basis) | 0,432 W | ⚠️ unbelastbar (s. o.) |
| station-only (kein SoftAP) | 0,298 W | ⚠️ unbelastbar (s. o.) |
| station-only + radio-save 5000/30 | **0,068 W** | ⚠️ unbelastbar (s. o.) |
| station-only + light sleep (radio aus) | 0,024 W | **USB-Konsole starb** |
| station-only + light sleep + radio-save 5000/30 | 0,028 W | **USB-Konsole starb** |

**Kernaussage (qualitativ, weiterhin gueltig):** Light Sleep schaltet die
USB-Serial-JTAG-Konsole des C6 ab und ist deshalb auf diesem Ziel unbrauchbar
(Abschnitt 8.1); Radio-Sleep bleibt als sicherer Hebel erhalten. Die
**Quantifizierung** der Einsparung ist mit der oben beschriebenen
Reihenmessung noch zu erbringen.
