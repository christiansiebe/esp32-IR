# C6-Empfänger: ESP-NOW-Re-Anchor bei Kanalwechsel (Roaming)

Stand: 2026-11-15 · Branch `feat/c6-rwdt-backstop` · Fix-Commit `3f21ca3` ·
am Gerät `192.168.178.105` (C6-Empfänger) reproduziert.

## Das Problem in einem Satz

Ein ESP-NOW-Peer **latcht den Funk-Kanal**, auf dem er angelegt wurde; eine
spätere STA-Assoziation auf einem **anderen** Kanal verschiebt ihn **nicht** —
roamt der Empfänger im Mesh auf einen anderen Kanal, geht der Empfang **still**
verloren, obwohl der Sender ununterbrochen sendet.

## Symptom (live, `.105`)

- `crc_valid` **friert ~200 s ein**, danach ein **+28-Frame-Schwall**.
- Das sah wie eine **187-s-Publish-Kadenz** aus: die HA-Recorder-Deltas waren
  **187 / 187 / 1 / 187** (Sekunden zwischen den Publish-Punkten).
- Kein Disconnect, kein Reset, kein Log — der Link „starb leise".
- **Folgeschaden:** Der `ps_fallback`-Wächter zählte die Stille als
  Modem-Sleep-Schaden und latschte **`ps off` sticky** — der Sparmodus war weg,
  obwohl der PS-Modus unschuldig war (siehe `docs/c6-modem-sleep-safety.md`).

## Ursache (Peer-Kanal-Anker)

- Der Empfänger läuft als APSTA; ESP-NOW nutzt `ESPNOW_IF` (im APSTA-Betrieb die
  **AP**-Schnittstelle, sonst STA).
- Ein Peer speichert den Kanal, auf dem `esp_now_add_peer()` lief. Ändert sich
  der Funk-Kanal der Schnittstelle später (Roaming), folgt der **Peer** nicht.
- Umgebung: FRITZ!Box-Mesh mit **zwei BSSIDs derselben SSID „Wohnzimmer"** auf
  **ch 11 <-> ch 1** (RSSI −84…−89 dBm). Der Empfänger wird zwischen ihnen hin-
  und hergereicht → der Peer-Anker passt nicht mehr zum aktiven Kanal → Funk
  empfängt nichts mehr.

Das ist **nicht** derselbe Fehler wie der ältere `OFFEN (2026-10-21)`-Abschnitt
(„Sender folgt dem Router-Kanal nicht"); hier ist es die **Empfänger-Seite**.

## A/B-Beweis

`set ap on` / `set ap off` ruft intern `espnow_migrate_peers()` auf: die
registrierten Peers (Broadcast, gelernter Sender, OTA-Ziel) werden **gelöscht und
neu angelegt** (mit `ifidx = ESPNOW_IF`, `channel = 0`). Das Re-Add **allein**:

| Zustand | Ergebnis |
|---|---|
| vorher (gelatchter Peer) | `crc_valid` **187 s eingefroren**, dann +28-Burst |
| nach `set ap on`/`off` (Re-Anchor) | **stetig ~3 Frames/s**, hielt **250 s**, **0 Einfrierungen** |

Die STA blieb dabei durchgehend auf **Kanal 1** — der Unterschied ist also
**allein** der neu gesetzte Peer-Anker, nicht der Kanal der Station und nicht der
Modem-Sleep.

## Fix (`3f21ca3`)

- **Code-Stelle:** `main/main.c`, `WIFI_EVENT_STA_CONNECTED`-Handler in
  `wifi_event_handler()` (Empfänger-Pfad, `#if !CONFIG_ESPNOW_ROLE_SENDER`).
- Vor dem Überschreiben von `g_espnow_channel` wird der **vorherige** Kanal
  gemerkt; landet die Assoziation auf einem **anderen** Kanal, wird
  `espnow_migrate_peers(ESPNOW_IF)` aufgerufen und `g_espnow_reanchors`
  inkrementiert:

  ```c
  const uint8_t previous_channel = g_espnow_channel;
  g_espnow_channel = event->channel;
  if (event->channel != previous_channel) {
      espnow_migrate_peers(ESPNOW_IF);
      ++g_espnow_reanchors;
      ESP_LOGW(TAG, "ESP-NOW peers re-anchored after channel change %u -> %u",
               (unsigned)previous_channel, (unsigned)event->channel);
  }
  ```

- `espnow_migrate_peers()` selbst (`main/main.c`, bei den Funk-Knöpfen) ist
  **unverändert**; sie wird nur jetzt auch aus dem STA-CONNECTED-Handler
  gerufen. Der **Sender-Pfad ist unangetastet**.

## Neuer Zähler `espnow_reanchors`

- `/api/dashboard`: JSON-Feld `espnow_reanchors` (neben `wifi_reconnects` /
  `wifi_reconnect_skips`).
- `status`-Zeile: `esp-now re-anchors: %u (peer channel moved with the STA)`.

Deutung: ein **steigender** Wert = der Re-Anchor greift (das frühere stille
Einfrieren wäre ohne ihn aufgetreten).

## Offene Punkte

- **Hardware-Nachweis unter echter Senderlast** fehlt — am Prüfstand ist kein
  Sender (`crc_valid=0`, `192.168.178.70` down); die Verlustfreiheit **nach** dem
  Re-Anchor ist damit nicht belegt.
- **Nachweis, dass `espnow_reanchors` bei echtem Roaming steigt:** ein
  erzwungener Mesh-Kanalwechsel (ch 11 <-> ch 1) mit **laufendem** Sender und
  Beobachtung des Zählers steht aus.
- Erst danach bewerten, ob der `ps_fallback`-Wächter die 187-s-Stille künftig
  sicher als **Link-Stille** statt als Modem-Sleep-Schaden einordnen kann (er
  sieht nur Disconnects; das Roaming liefert keinen).
