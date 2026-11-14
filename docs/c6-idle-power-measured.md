# C6-Receiver: gemessene Idle-Leistung (FNB-48, 2026-11-14)

Messergebnisse am aufgebauten ESP32-C6FH4 (USB, `/dev/ttyACM0`), Speisung über
den **FNIRSI FNB-48** (USB-HID `0483:003a`, `/dev/hidraw0`). Messung mit
`tools/fnb48_read.py -d /dev/hidraw0 -n 15`; angegeben ist der `avg`-Wert
(und `min`, wenn der Parkzustand interessiert). Board: `v0.4.0-45-g7547f82`.

> **Kein Sender am Prüfstand.** `crc_valid=0`, `sender_rssi_valid=false`,
> `192.168.178.70` antwortet nicht. Alle Zahlen sind **reine Idle-/Empfangs-
> bereitschaft**, die Verlustfreiheit unter echter Senderlast ist damit **nicht**
> belegt (offene Aufgabe, s. TODO).

## Kernbefund: der SoftAP kostet ~0,22 W

Zwei Images, **identische Laufzeit-Knobs** (`cpu=80`, `radio-save=5000/30`,
`ps=max`, `listen=10`, Burst-Lock aus), nur der WLAN-Modus unterscheidet sich:

| Image | WLAN-Modus | Idle Ø | Idle min |
|---|---|---|---|
| `receiver-c6` (Default) | `WIFI_MODE_APSTA` (SoftAP an, main.c:1440) | **0,42 W** | 0,41 W |
| `receiver-c6-apoff` | `WIFI_MODE_STA` (SoftAP aus, main.c:1436) | **0,19 W** | 0,12 W |

Der **laufende Setup-SoftAP (Beacons + AP-MAC/ARP, `max_connection=4`) ist der
dominante Idle-Verbraucher** und kostet rund **0,22 W**. Ein Beacon-Intervall
von 1000 TU (`set beacon 1000`) ändert daran nichts (0,44 W) — es ist nicht die
Beacon-Rate, sondern der AP-Betrieb selbst.

## `esp_wifi_set_ps()` wirkt am Receiver **nicht**

Auf dem APSTA-Image gemessen, Leiter aus, sonst gleiche Knobs:

| `set ps` | Ø |
|---|---|
| `max` | 0,425 W |
| `min` | 0,428 W |
| `off` | 0,424 W |

Praktisch **kein Unterschied**. Ursache (in `docs/c6-modem-sleep-safety.md`
schon notiert): `esp_wifi_set_ps()` parkt nur die **STA**-Schnittstelle. Am
APSTA-Receiver läuft der ESP-NOW-Empfang über die **SoftAP**-Schnittstelle
(`ESPNOW_IF = WIFI_IF_AP`), die vom Modem-Sleep nicht erfasst wird. Deshalb
kann `ps max`/die Leiter die APSTA-Idle-Leistung **nicht** senken — sie schützt
nur den Fall, in dem die PS-Sprosse tatsächlich greift (z. B. spätere
sta-only-Variante bzw. Link ohne AP).

## CPU-Takt ist ein kleiner Hebel

| `set freq` | Ø |
|---|---|
| 160 MHz | 0,439 W |
| 80 MHz | 0,423 W |
| 40 MHz | 0,423 W |

160→80 spart ~0,016–0,036 W (~4–8 %), 80→40 nichts. Erwartungsgemäß: die
Radio-Dominanz überdeckt die CPU.

## `radio-save` / Burst-Lock (nur apoff relevant)

Auf dem **apoff**-Image (dort greift der Modem-Sleep) ist `radio-save` der
wirksame Hebel:

| Konfiguration | Ø | min |
|---|---|---|
| burst-lock on, radio-save 5000/30, **innerhalb** 60-s-Lernfenster | 0,40 W | 0,39 W |
| burst-lock on, radio-save 5000/30, **nach** Lernfenster | 0,194 W | 0,116 W |
| burst-lock off, radio-save 20/15 | 0,395 W | 0,386 W |
| burst-lock off, radio-save 100/50 | 0,258 W | 0,228 W |
| burst-lock off, radio-save 1000/50 | 0,194 W | 0,120 W |
| burst-lock off, radio-save 5000/30 | 0,192 W | 0,120 W |

Zwei Erkenntnisse:

1. **Burst-Lock hält die Funkstunde initial wach.** Ohne Sender lernt der
   Burst-Lock nie einen Burst und hält das Modem bis zum
   `BURST_LOCK_LEARN_TIMEOUT_MS` (60 s) auf „awake" (`burst_awake_pct` bis 58 %).
   Danach fällt er auf „stay awake and re-learn" → das Modem parkt
   (0,40 → 0,19 W). **Die 0,40 W direkt nach dem Flashen sind transient.**
2. **Ohne Sender ist Burst-Lock an ≈ aus**, sobald das Lernfenster zu ist —
   im Sendebetrieb ist „an" aber nötig, damit der 5-s-Burst nicht verpasst wird.
   Ein pauschales „Burst-Lock aus" ist also **keine** gültige Empfehlung.

## Empfehlung

- **Für minimale Idle-Leistung den `receiver-c6-apoff` (station-only) flashen.**
  Der Standard-`receiver-c6` zahlt ~0,22 W für den SoftAP-Fallback
  (`192.168.4.1`), der nur beim Erst-Setup ohne FRITZ!Box-Zugang gebraucht wird.
- Sobald die WLAN-Zugangsdaten persistiert sind, ist der SoftAP entbehrlich —
  ein sinnvoller Kompromiss wäre, den AP nach erfolgreicher STA-Verbindung
  abzuschalten (Kandidat, siehe TODO).
- `ps max` + Leiter bleiben korrekt und schaden nicht; sie sind am APSTA-Receiver
  nur **wirkungslos** für die Idle-Leistung.

## Umgesetzt (2026-11-14): automatischer SoftAP-Abschalter

Der SoftAP ist jetzt **kein Dauerläufer** mehr, sondern ein Setup-Fenster:

- Neuer persistierter Knopf **`ap_enabled`** (SoftAP an/aus) und **`ap_auto_off`**
  (nach STA-Verbindung automatisch abschalten). Blob **v9**, 26 B (`[24]`/`[25]`).
- **Default am C6-Receiver:** `ap_enabled=true`, `ap_auto_off=true` → der AP ist
  beim Boot da (Erst-Setup + Kanal-Scan des Senders) und fällt weg, sobald die
  Station eine IP hat. Der klassische ESP32-Empfänger bleibt unverändert
  (`ap_auto_off=false`); der `apoff`-Build startet station-only (`ap_enabled=false`).
- **Bedienung:** `set ap on|off`, `set ap-auto-off on|off`; Seite `/config`
  (Select + „Setup-AP übernehmen" → POST `/api/ap`); `/api/dashboard` liefert
  `ap_enabled`/`ap_auto_off`.
- **Sicherheit:** Der AP lässt sich nur abschalten, solange die Station verbunden
  ist (sonst HTTP 400 / Konsolenfehler, der Knopf wird zurückgerollt). ESP-NOW-Peers
  werden beim Umschalten von `WIFI_IF_AP` auf `WIFI_IF_STA` migriert, damit der Link
  den Wechsel überlebt. Ist der Sender still, holt der Link-Fallback den AP zurück
  (Selbstheilung) — das kostet die ~0,22 W wieder, hat aber Vorrang.

## Reproduzieren

```sh
python3 tools/fnb48_read.py -d /dev/hidraw0 -n 15        # Ø/min/max der Leistung
# Knobs am Gerät (USB-Konsole, 115200):
#   set ps max|min|off   set radio-save on|off   set wake-interval <ms>
#   set wake-window <ms> set burst-lock on|off   set freq 160|80|40
#   set beacon <TU>      set ps-ladder on|off
```
