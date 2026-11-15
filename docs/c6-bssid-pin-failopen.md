# C6-Empfänger: Sticky-BSSID-Pin mit Fail-Open (Roaming verhindern)

Stand: 2026-11-15 · Branch `feat/c6-rwdt-backstop` · Commit `63c24e3` ·
Gerät `192.168.178.105` (C6-Empfänger).

## Das Problem in einem Satz

Der Re-Anchor-Fix (`3f21ca3`, `docs/c6-espnow-roaming-reanchor.md`) **heilt** den
Kanalwechsel — aber jede Mesh-Übergabe kostet weiter Frames, weil die
kanalgebundenen ESP-NOW-Peers bei jedem Roaming neu verankert werden müssen.

## Die Ursache entfernen statt heilen

Die FRITZ!Box veröffentlicht eine SSID über **mehrere BSSIDs auf
verschiedenen Kanälen**. Ein schwacher Client (−84…−89 dBm) wird zwischen ihnen
hin- und hergereicht, und **jede** Übergabe verschiebt den Funk-Kanal. ESP-NOW
ist kanalgebunden, also reißt jede Übergabe den Sender-Link.

**Sticky-BSSID-Pin:** Der Empfänger merkt sich die BSSID, mit der er sich zuerst
assoziiert hat, und pinnt sich darauf (`wifi_config_t.sta.bssid` +
`bssid_set = true`). Das Mesh kann den Client — und damit den ESP-NOW-Kanal —
nicht mehr bewegen.

## Fail-Open: die Verfügbarkeitsgarantie

Betriebspriorität: **Der Empfänger darf lieber den Sender verlieren als selbst
offline gehen.** Deshalb ist der Pin nie eine Sackgasse:

| Ereignis | Verhalten |
|---|---|
| Assoziation gelingt | Pin re-armiert (Fail-Open-Uhr zurückgesetzt) |
| Pinned BSSID weg (`WIFI_REASON_NO_AP_FOUND`) | Pin **sofort** freigegeben |
| Sonst getrennt, länger als `ESPNOW_BSSID_PIN_FAILOPEN_MS` ohne Assoziation | Pin freigegeben |
| Pin freigegeben | Nächster Connect darf **jede** BSSID nehmen |

Nach einer Freigabe pinnt sich der nächste erfolgreiche Connect automatisch
wieder auf die neue BSSID. Der Empfänger kann sich also **nie** auf einer toten
BSSID festfahren.

## Code

- **Modul** `main/bssid_pin.{c,h}` — reines C, ohne ESP-IDF, hostgetestet
  (`test/test_bssid_pin.c`).
- **`main.c`** (`#if !CONFIG_ESPNOW_ROLE_SENDER`):
  - `initialize_wifi_and_espnow()`: `bssid_pin_init()` + `load_sta_bssid()` vor
    der ersten Assoziation.
  - `fill_station_config()`: setzt `sta.bssid`/`sta.bssid_set` aus dem Pin.
  - `WIFI_EVENT_STA_CONNECTED`: `bssid_pin_note_connected()` — nimmt den Pin
    beim ersten Connect (nur wenn `ESPNOW_BSSID_PIN`), persistiert ihn per
    `save_sta_bssid()`.
  - `WIFI_EVENT_STA_DISCONNECTED`: `bssid_pin_note_disconnect()` — Fail-Open;
    bei Freigabe `clear_sta_bssid()` + Konfig neu setzen, dann reconnecten.
- **NVS** (`WIFI_CONFIG_NAMESPACE`, Key `sta_bssid`): der Pin überlebt einen
  Reboot, damit die Anti-Roaming-Entscheidung nicht verloren geht.
- **Konsole:** `set bssid <aa:bb:cc:dd:ee:ff>` / `set bssid off` (manueller
  Override; ein redundantes `set` wirft den Link **nicht**).
- **Anzeige:** `wifi`- und `status`-Zeile, plus JSON-Felder `bssid_pinned`,
  `bssid_pin_releases`, `bssid_pin_skips` in `/api/dashboard`.
- **Kconfig:** `ESPNOW_BSSID_PIN` (Default `y`) schaltet das automatische
  Pinnen ab; `ESPNOW_BSSID_PIN_FAILOPEN_MS` (Default `60000`) ist das
  Fail-Open-Fenster (`0` = Freigabe beim ersten Disconnect).

## Zähler deuten

- `bssid_pinned` = aktuelle Pin-BSSID (`none` = kein Pin, freies Roaming).
- `bssid_pin_releases` steigt = der Fail-Open-Schutz hat gegriffen.
- `espnow_reanchors` bleibt bei aktivem Pin **0** (kein Roaming → kein
  Re-Anchor nötig); steigt er doch, hat das Mesh trotz Pin den Kanal bewegt.

## Offene Punkte

- **Feldnachweis** der Langzeitstabilität mit aktivem Pin (mehrere Stunden) steht
  aus; ein erzwungener Mesh-Kanalwechsel mit gesetztem Pin wäre der direkte
  Gegenbeweis zum Roaming-Symptom.
- Bewerten, ob der Pin im Mesh einen **kleineren RSSI** in Kauf nimmt
  (festgehalten an einer schwächeren BSSID) — gegen den Gewinn eines stabilen
  ESP-NOW-Kanals abzuwägen.
