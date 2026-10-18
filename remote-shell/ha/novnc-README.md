# XFCE-Desktop im Browser (Odroid N2+)

Der N2+ läuft mit **Armbian bookworm** und einem echten XFCE-Desktop, der über
**TigerVNC** (Anzeige `:1`, Port `5901`) bereitsteht. **noVNC + websockify**
übersetzen VNC nach WebSocket, damit der Desktop im Browser bedienbar ist.

```
Browser (/vnc.html) <-> websockify :6080 <-> TigerVNC 127.0.0.1:5901 <-> XFCE
                              |
                    Paketfilter VNC_LAN (nur LAN + Loopback)
```

## Aufruf

```
http://odroidn2.local:6080/vnc.html      (oder http://192.168.178.71:6080/vnc.html)
```

**`/vnc.html` nicht weglassen.** `http://odroidn2.local:6080/` allein liefert die
**noVNC-Verzeichnisliste** von `/usr/share/novnc/` — die sieht wie ein
Datei-Explorer aus, ist aber nicht der Desktop. Genau dieser Aufruf hat früher
den Eindruck „nur File Explorer" erzeugt.

**Kein Passwort** — auf Wunsch des Nutzers. Der Desktop meldet sich also sofort.
Der Schutz ist stattdessen die **Quell-IP-Prüfung**: nur Geräte aus dem eigenen
LAN (und der Rechner selbst über Loopback) erreichen Port `6080`. Alles andere
wird verworfen. Zwei Schichten:

1. **TigerVNC hört nur auf `127.0.0.1`** (`-localhost yes`). Port `5901` ist von
   außen gar nicht erreichbar; nur `websockify` auf demselben Host kommt dran.
2. **`vnc-lan-guard.service`** hält die iptables-Kette `VNC_LAN` in `INPUT`, die
   auf Port `6080` nur `192.168.178.0/24` und Loopback durchlässt, sonst `DROP`.

> **Voraussetzung:** Das LAN muss vertrauenswürdig sein. Ohne Passwort hat jeder
> im LAN (auch Gäste-IoT, ein gekapertes Gerät) vollen **root**-Desktopzugriff.
> Wer das nicht will, setzt in `xfce-vnc.service` wieder
> `-SecurityTypes VncAuth` und lässt `deploy_novnc.sh` erneut laufen.

## Dateien

- `xfce-vnc.service` — TigerVNC `:1`, `1920x1080`, `-localhost yes`
  (nur Loopback), `-SecurityTypes None` (kein Passwort).
- `novnc.service` — `websockify --web=/usr/share/novnc 0.0.0.0:6080 127.0.0.1:5901`,
  `Requires=xfce-vnc.service`, `Restart=always`.
- `vnc-lan-guard.service` — der Paketfilter (eigene Kette `VNC_LAN`).
  `After=netfilter-persistent.service` (die lädt beim Boot `rules.v4` und würde
  die Kette sonst entfernen), `Before=novnc.service`, plus synchroner
  `ExecStartPre=… --check`. Heilt sich zusätzlich alle 30 s selbst.
- `vnc_lan_guard.py` — die Logik (host-getestet, `test_vnc_lan_guard.py`).
  `--print` zeigt die Soll-Regeln, `--remove` nimmt die Kette weg (Notausgang).
- `vnc-lan-guard.default` — `/etc/default/vnc-lan-guard`: `LAN_CIDR`, `PORT`, …
- `vnc-xstartup` — `~/.vnc/xstartup`: `exec dbus-launch --exit-with-session startxfce4`
  (D-Bus ist Pflicht, sonst startet XFCE nicht sauber).
- `deploy_novnc.sh` — installiert Pakete, Units, xstartup (mit Passwort, Standard).
- `deploy_vnc_lan.sh` — **der LAN-only/passwortlos-Umbau** (idempotent):
  `sh deploy_vnc_lan.sh [LAN-CIDR]`.

## Installieren

Standard (mit Passwort):

```bash
sh deploy_novnc.sh
```

Passwortlos, nur LAN (der aktuelle Stand):

```bash
sh deploy_vnc_lan.sh 192.168.178.0/24
```

Pakete: `tigervnc-standalone-server xfce4 dbus-x11 novnc websockify`.

## Prüfen

```bash
systemctl is-active xfce-vnc.service novnc.service vnc-lan-guard.service
# 5901 MUSS 127.0.0.1 sein (nicht 0.0.0.0):
ss -tlnp | grep -E ':5901|:6080'
# Regeln:
iptables -S VNC_LAN
# Kein Passwort angeboten? (1 = None)
python3 -c "import socket;s=socket.create_connection(('127.0.0.1',5901));v=s.recv(12);s.sendall(v);n=s.recv(1)[0];print(list(s.recv(n)))"
```

Negativtest (eine Quelle außerhalb des LAN wird verworfen) über eine
Netz-Namespace — `10.99.99.2` läuft in einen Timeout, `192.168.178.240` (LAN)
bekommt `HTTP 200`:

```bash
ip netns add vnctest && ip link add veth0 type veth peer name veth1
ip link set veth1 netns vnctest && ip addr add 10.99.99.1/24 dev veth0 && ip link set veth0 up
ip netns exec vnctest ip addr add 10.99.99.2/24 dev veth1
ip netns exec vnctest ip addr add 192.168.178.240/32 dev veth1
ip netns exec vnctest ip link set veth1 up && ip netns exec vnctest ip link set lo up
ip netns exec vnctest ip route add default via 10.99.99.1
ip route add 192.168.178.240/32 dev veth0
ip netns exec vnctest curl -s -o /dev/null -w 'nicht-LAN: %{http_code}\n' --max-time 5 --interface 10.99.99.2 http://192.168.178.71:6080/vnc.html
ip netns exec vnctest curl -s -o /dev/null -w 'LAN: %{http_code}\n' --max-time 5 --interface 192.168.178.240 http://192.168.178.71:6080/vnc.html
```

Erwartet: `nicht-LAN: 000` (verworfen), `LAN: 200`.

## Nicht verwechseln

- **Port 8099** ist NICHT der Desktop: dort läuft `meter_server.py`
  (fritz-meter, Live-JPEG des ESP32-Dashboards).
- **Port 7681** ist der serielle IDF-Monitor im Browser (`idf-monitor-web/`).
- `:6080/` ohne `/vnc.html` = noVNC-Verzeichnisliste (sieht wie ein File-Manager
  aus). Der Desktop ist `:6080/vnc.html`.

