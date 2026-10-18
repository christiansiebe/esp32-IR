# Odroid N2 — Netz-Recon (AP übernehmen + Kamera über LAN)

Stand: 2026-10-18, erhoben über den Kommando-Kanal (`remote-shell/cmd_send.sh`).
Board: Odroid N2, Armbian bookworm, `odroidn2.local` / 192.168.178.71.

## Ist-Zustand

| Punkt | Befund |
|-------|--------|
| Ethernet `end0` (MAC `00:1e:06:42:b1:25`) | **DOWN / NO-CARRIER** — das LAN-Kabel steckt **nicht** am Odroid (es hängt am AP). |
| WLAN `wlxdc627949ba79` (USB, `rtw88_8822bu`, TP-Link 802.11ac) | **UP**, `managed`, SSID **"Wohnzimmer 5 Ghz"** (Kanal 36), IP `192.168.178.71/24`. |
| Default-Route | `via 192.168.178.1` (FritzBox, DHCP+DNS) über WLAN. |
| Gateway `192.168.178.1` | erreichbar, 1,7 ms. |
| `192.168.178.4` (frühere Odroid-IP / evtl. alter AP) | nicht erreichbar. |
| `ip_forward` | **1** (bereits aktiv). |
| `hostapd` / `dnsmasq` | **inzwischen installiert** (`2:2.10-12+deb12u3` bzw. `2.90-4~deb12u2`) und **masked** — starten nicht von selbst (Nachtrag 2026-10-18, s. u.). |
| `nmcli`, `bridge`, `iptables`, `ffmpeg` | vorhanden. |
| `/dev/video0` | **keine Kamera** — nur der meson-HW-Video-Decoder (`ff620000.video-decoder`). Kein V4L2-Kameragerät. |

## WLAN-Chip: AP-Modus ja, AP+STA **unsicher**

`iw list` für `wlxdc627949ba79`:
```
Supported interface modes:
        * IBSS
        * managed
        * AP
        * AP/VLAN
        * monitor
```
Der AP-Modus wird also unterstützt. **Aber**: `iw list` listet **keine**
`valid interface combinations` — der rtw88-Treiber meldet keine erlaubte
Kombination. Damit ist **nicht garantiert**, dass der Chip gleichzeitig als AP
(für die alten AP-Clients) **und** als STA (im Heimnetz) laufen kann.

⚠️ **Lockout-Risiko:** Es gibt nur **ein** Funkmodul. Schaltet man das WLAN
heute in den AP-Modus, fällt die STA-Verbindung ins Heimnetz weg — dann ist der
Odroid (bis `end0` als Uplink steht) über das Netz **nicht mehr erreichbar**
(auch der Kommando-Kanal nicht, der über die WLAN-STA-Verbindung läuft).

## Empfohlene Zielarchitektur

1. **`end0` = Uplink ins Heimnetz.** LAN-Kabel von der FritzBox (bzw. dem
   Switch) in den Odroid-Ethernet-Port. Dann ist der Odroid **kabelgebunden**
   im Heimnetz — unabhängig vom WLAN und robust für den Umbau.
2. **`wlxdc627949ba79` = AP** für die Geräte, die vorher am alten AP hingen:
   `hostapd` (WLAN) + `dnsmasq` (DHCP) + NAT/Bridge auf `end0`
   (`net.ipv4.ip_forward=1` ist schon gesetzt).
3. **Kamera über LAN:** IP-Kamera (RTSP/ONVIF) an denselben Switch wie `end0`.
   Einbindung in **Home Assistant** (läuft auf dem Odroid) über die
   ONVIF-/Generic-Camera-Integration. USB-UVC ist derzeit nicht möglich
   (kein Kamera-V4L2-Gerät vorhanden; `ffmpeg` für RTSP→HA ist da).

Reihenfolge ist zwingend: **erst `end0` als Uplink verifizieren, dann WLAN auf
AP umstellen** — sonst sperrt man sich aus.

## Offene Entscheidungen (Nutzer)

- Welcher AP ist das genau (Modell), und soll der Odroid ihn **ersetzen** oder
  nur **ergänzen**?
- Kamera: **IP-Kamera** (RTSP/ONVIF, über LAN) oder **USB-Kamera**? Bei IP:
  Modell/Hersteller, damit die HA-Integration passt.
- Soll der Odroid reiner **AP** werden oder **Router** (eigenes Subnetz + NAT)?

## Nachtrag 2026-10-18 (Parent, Re-Verifikation am Board)

Der Kommando-Kanal war nach dem LAN-Umstecken wieder erreichbar (der Odroid
verbindet sich per `Restart=always` selbst). Erneut gelesen, **read-only**:

- **`end0` weiterhin `DOWN` / `carrier=0`** — das Kabel steckt noch nicht am
  Odroid. Damit ist **P1 (end0-Uplink) nicht begonnen**; P2 (WLAN→AP) bleibt
  gesperrt (Lockout-Gefahr).
- **WLAN-STA** `wlxdc627949ba79`: `connected`, SSID **"Wohnzimmer 5 Ghz"**,
  `freq 5180` (Kanal 36), **Signal −33 dBm**, TX 866,7 / RX 780 MBit/s
  (VHT-MCS9, 80 MHz). Also die **FRITZ!Box** — nicht der zu ersetzende AP.
  `nmcli`: `wlxdc627949ba79:wifi:connected`, `end0:ethernet:unavailable`.
- **Sichtbare SSIDs** (Scan): `Wohnzimmer:1` (2,4 GHz), `Wohnzimmer 5 Ghz:36`,
  `ESP32-IR-Meter:1` (der Sender-AP). Ein **fremder** AP mit eigener SSID taucht
  nicht auf — der „alte AP" ist vermutlich in das „Wohnzimmer"-Netz gemesht,
  nicht als eigener Name. Das stützt die offene Frage „ersetzen oder ergänzen".
- **`hostapd` + `dnsmasq` installiert, beide `masked`** (`is-active: inactive`) —
  bereit für die echte Konfiguration, starten nicht von selbst.
- **USB-WLAN** = `2357:0138 TP-Link 802.11ac NIC` (`rtw88_8822bu`); zweiter
  USB-Steckplatz belegt durch `0bda:9210` (RTL9210 M.2-NVMe-Gehäuse).
- **`/dev/video0`** weiterhin nur der Amlogic-Decoder; `v4l2-ctl` ist **nicht**
  installiert (kein Kameragerät, also erwartbar). Keine Kamera angeschlossen.
- **HA-Container** `homeassistant` läuft (Docker), `ip_forward=1` unverändert.
- `user_overlays=lowfreq ramoops` (kein OC-Overlay), `xfce-vnc`/`novnc`/
  `vnc-lan-guard` alle **active**.
