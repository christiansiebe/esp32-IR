# XFCE-Desktop im Browser (Odroid N2+)

Der N2+ läuft mit **Armbian bookworm** und einem echten XFCE-Desktop, der über
**TigerVNC** (Anzeige `:1`, Port `5901`) bereitsteht. **noVNC + websockify**
übersetzen VNC nach WebSocket, damit der Desktop im Browser bedienbar ist.

```
Browser (/vnc.html) <-> websockify :6080 <-> TigerVNC :5901 <-> XFCE (startxfce4)
```

## Aufruf

```
http://odroidn2.local:6080/vnc.html      (oder http://192.168.178.71:6080/vnc.html)
```

**`/vnc.html` nicht weglassen.** `http://odroidn2.local:6080/` allein liefert die
**noVNC-Verzeichnisliste** von `/usr/share/novnc/` — die sieht wie ein
Datei-Explorer aus, ist aber nicht der Desktop. Genau dieser Aufruf hat früher
den Eindruck „nur File Explorer" erzeugt.

VNC-Passwort: liegt auf dem Odroid in `/root/.vnc/PASSWORT.txt` (0600, nur Host).
Nicht im Repo (öffentlich).

## Dateien

- `xfce-vnc.service` — TigerVNC `:1`, `1920x1080`, `-localhost no`, VncAuth.
- `novnc.service` — `websockify --web=/usr/share/novnc 0.0.0.0:6080 localhost:5901`,
  `Requires=xfce-vnc.service`, `Restart=always`.
- `vnc-xstartup` — `~/.vnc/xstartup`: `exec dbus-launch --exit-with-session startxfce4`
  (D-Bus ist Pflicht, sonst startet XFCE nicht sauber).
- `deploy_novnc.sh` — installiert Pakete, Units, xstartup, Passwort (idempotent).

## Installieren

```bash
sh deploy_novnc.sh
```

Pakete: `tigervnc-standalone-server xfce4 dbus-x11 novnc websockify`.

## Prüfen

```bash
systemctl is-active xfce-vnc.service novnc.service
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:6080/vnc.html   # 200
# WebSocket-Handshake + RFB-Greeting:
curl -s -i -N -o - --max-time 3 \
  -H 'Upgrade: websocket' -H 'Connection: Upgrade' \
  -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' -H 'Sec-WebSocket-Version: 13' \
  http://localhost:6080/websockify | head
```

Erwartet: `101 Switching Protocols`, danach das RFB-Greeting `RFB 003.00x` vom
VNC-Server.

## Nicht verwechseln

- **Port 8099** ist NICHT der Desktop: dort läuft `meter_server.py`
  (fritz-meter, Live-JPEG des ESP32-Dashboards).
- **Port 7681** ist der serielle IDF-Monitor im Browser (`idf-monitor-web/`).
- `:6080/` ohne `/vnc.html` = noVNC-Verzeichnisliste (sieht wie ein File-Manager
  aus). Der Desktop ist `:6080/vnc.html`.
