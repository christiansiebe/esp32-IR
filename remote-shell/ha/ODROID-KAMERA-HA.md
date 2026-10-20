# Odroid N2 — Kamera über LAN in Home Assistant einbinden (T4, Runbook)

Stand: 2026-10-20 (T4). Ergänzt den Umsetzungsplan
[`ODROID-LAN-AP-KAMERA.md`](ODROID-LAN-AP-KAMERA.md) (Branch
`cline/power-history-diag`) um die **konkreten HA-Schritte**, sobald eine Kamera
vorhanden ist.

## Voraussetzung (Nutzer-Entscheid, blockiert die Umsetzung)

T4 ist ein **Zukunftsvorhaben**. Es fehlt die Hardware-Entscheidung:

- **Kamera-Typ:** IP (RTSP/ONVIF) — empfohlen — oder USB/UVC?
- **Modell + Zugangsdaten** (für die RTSP/ONVIF-URL).
- **Anschluss:** Kamera am selben Switch/FRITZ!Box wie `end0` (empfohlen, kein
  zweiter NIC nötig) oder direkt am Odroid (braucht USB-Ethernet + Bridge)?

Ohne diese Angaben ist nur die **Vorbereitung** möglich (dieses Runbook), nicht
das Andocken.

## Empfohlener Weg: IP-Kamera über LAN (ONVIF)

Der Odroid hat **einen** Ethernet-Port (`end0`). Hängt die Kamera am selben
Heimnetz (Switch/FRITZ!Box), braucht der Odroid **keinen** zweiten NIC — er
erreicht die Kamera-IP wie jedes andere LAN-Gerät. Der HA-Container läuft mit
normalem Docker-Netz und darf ausgehend ins LAN (bei `network_mode: host`
trivial).

### Schritt 1 — Kamera im LAN finden

Im Heimnetz (nicht über den Odroid-Kanal; der erlaubt kein `nmap`/`ip`):

```sh
# ONVIF-Discovery (WS-Discovery, UDP 3702) — vom HA-Host aus
python3 -c "import socket; print('ONVIF-Discovery ueber HA-Integration)')"
# oder einfach in HA: Einstellungen -> Geräte & Dienste -> Integration hinzufügen
#   -> "ONVIF" -> sucht selbst per WS-Discovery
```

### Schritt 2 — HA-Integration ONVIF (empfohlen)

1. HA → **Einstellungen → Geräte & Dienste → Integration hinzufügen → ONVIF**.
2. Eingabe: **IP**, **ONVIF-Port** (meist `80` oder `8000`), **User/Passwort**.
3. Ergebnis: Videostream **plus** Motion-/Tamper-Events (falls die Kamera sie
   anbietet). Diese Events lassen sich direkt in Automationen nutzen.

### Schritt 3 — HA-Integration Generic Camera (Rückfall, wenn ONVIF nicht passt)

Manuelle RTSP-URL (Muster herstellerspezifisch):

```yaml
# configuration.yaml
camera:
  - platform: generic
    name: "Hof"
    stream_source: "rtsp://user:pass@192.168.178.50:554/Streaming/Channels/101"
    still_image_url: "http://user:pass@192.168.178.50/Streaming/Channels/101/picture"
    authentication: basic
    verify_ssl: false
```

Typische RTSP-Muster: `/Streaming/Channels/101` (Hikvision),
`/stream1` (Reolink), `/h264Preview_01_main` (Amcrest).

### Schritt 4 — Erreichbarkeit prüfen (am Board, sofern erlaubt)

`ffmpeg` ist auf dem Odroid vorhanden (Recon). Ein Stream-Test bestätigt Bild
**und** Codec, bevor HA eingerichtet wird:

```sh
ffmpeg -rtsp_transport tcp -i "rtsp://user:pass@<IP>:554/..." -t 5 -f null -
```

Über den Kommandokanal ist `ffmpeg` **nicht** allowlistet — dieser Test braucht
direkten Board-Zugang oder eine erweiterte Allowlist.

## Optional: Aufzeichnung / Objekterkennung

Für mehr als „nur ansehen" (Aufzeichnung, Personenerkennung, WebRTC):
**go2rtc** (HA-Add-on/Container) oder **Frigate**. Nicht nötig für die reine
Anzeige.

## USB/UVC-Kamera (Weg B) — nur wenn zwingend

- Heute unmöglich: `/dev/video0` ist der Amlogic-Decoder, **keine** Kamera
  (Recon). Erst nach Anstecken einer UVC-Kamera entsteht `/dev/videoN`.
- HA hat **keine** UVC-Integration. Wege: **Generic Camera** mit
  `-f v4l2 -i /dev/videoN` (ffmpeg), oder ein MJPEG-Brückendienst
  (`mjpg-streamer`).
- Container-Zugriff: Gerät durchreichen (`--device /dev/videoN`).
- Nachteil: an den Odroid gebunden, USB-Bandbreite (der Stick teilt sich den
  USB-2.0-Bus), schwächer für Daueraufzeichnung.

## Offene Punkte (T4)

- [ ] Nutzer-Entscheid: Kamera-Typ + Modell + Zugangsdaten + Anschlussort.
- [ ] Kamera ins LAN (Switch/FRITZ!Box) bzw. USB anstecken.
- [ ] ONVIF-Integration in HA hinzufügen, Bild prüfen.
- [ ] (Optional) Aufzeichnung mit go2rtc/Frigate.
