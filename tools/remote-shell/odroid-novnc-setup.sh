#!/bin/bash
# Odroid-Setup: echten XFCE-Desktop per Browser erreichbar machen (noVNC).
#
# Voraussetzung: XFCE + TigerVNC laufen bereits (xfce-vnc.service, Port 5901).
# Dieses Skript installiert noVNC + websockify und legt einen reboot-festen
# systemd-Dienst `novnc.service` an:
#     websockify --web=/usr/share/novnc/ 6080 localhost:5901
# Danach: http://odroidn2.local:6080/vnc.html
#
# Aufruf AUF DEM ODROID (als root):
#   bash odroid-novnc-setup.sh
set -euo pipefail

echo "=== 1. Pakete installieren ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y novnc python3-websockify

echo "=== 2. noVNC-Webroot pruefen ==="
WEBROOT=/usr/share/novnc
[ -d "$WEBROOT" ] || WEBROOT=/usr/share/webapps/novnc
[ -f "$WEBROOT/vnc.html" ] || { echo "FEHLER: vnc.html nicht in $WEBROOT"; exit 1; }
echo "Webroot: $WEBROOT"

echo "=== 3. systemd-Dienst novnc.service ==="
cat > /etc/systemd/system/novnc.service <<EOF
[Unit]
Description=noVNC (websockify: HTTP 6080 -> VNC 5901)
Documentation=https://github.com/novnc/noVNC
After=network-online.target xfce-vnc.service
Wants=network-online.target xfce-vnc.service

[Service]
Type=simple
ExecStart=/usr/bin/websockify --web=$WEBROOT 6080 localhost:5901
Restart=always
RestartSec=3
User=root

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now novnc.service
sleep 3
systemctl --no-pager --lines=5 status novnc.service || true

echo "=== 4. Ports ==="
ss -ltnp 2>/dev/null | grep -E ':(5901|6080)\b' || true

echo "=== 5. HTTP-Test ==="
curl -sS -o /dev/null -w "vnc.html -> HTTP %{http_code}\n" http://127.0.0.1:6080/vnc.html || true
