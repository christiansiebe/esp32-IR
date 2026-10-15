#!/bin/bash
# Odroid-Einmal-Setup: echten XFCE-Desktop per Browser (noVNC) einrichten.
#
# Fasst Diagnose + Installation + Verifikation in EINEM Lauf zusammen.
# Idempotent: mehrfaches Ausfuehren ist unschaedlich.
#
# Aufruf AUF DEM ODROID (als root):
#   bash odroid-setup-desktop.sh
set -u

echo "############ 1/4  Ist-Zustand ############"
for name in xfce-vnc novnc; do
  printf '%-9s enabled=%-9s active=%s\n' "$name" \
    "$(systemctl is-enabled $name.service 2>/dev/null)" \
    "$(systemctl is-active  $name.service 2>/dev/null)"
done
echo "-- Ports --"
ss -ltnp 2>/dev/null | grep -E ':(5901|6080|80|8080)\b' || echo "(5901/6080/80/8080 nicht offen)"

echo
echo "############ 2/4  noVNC + websockify installieren ############"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y novnc python3-websockify

WEBROOT=/usr/share/novnc
[ -f "$WEBROOT/vnc.html" ] || WEBROOT=/usr/share/webapps/novnc
if [ ! -f "$WEBROOT/vnc.html" ]; then
  echo "FEHLER: vnc.html nicht gefunden (weder /usr/share/novnc noch /usr/share/webapps/novnc)"; exit 1
fi
echo "Webroot: $WEBROOT"

echo
echo "############ 3/4  Dienst novnc.service ############"
cat > /etc/systemd/system/novnc.service <<EOF
[Unit]
Description=noVNC (websockify: HTTP 6080 -> VNC 5901)
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
systemctl --no-pager --lines=8 status novnc.service || true

echo
echo "############ 4/4  Verifikation ############"
ss -ltnp 2>/dev/null | grep -E ':(5901|6080)\b' || echo "(WARNUNG: 5901/6080 nicht offen)"
curl -sS -o /dev/null -w "  http://127.0.0.1:6080/vnc.html -> HTTP %{http_code}\n" http://127.0.0.1:6080/vnc.html || true
echo "  VNC-Passwort: $(cat /root/.vnc/PASSWORT.txt 2>/dev/null || echo '(nicht gefunden)')"

echo
echo "=== FERTIG ==="
echo "Im Browser oeffnen:  http://odroidn2.local:6080/vnc.html"
echo "Kandidat 'File Explorer' (Web-Dienst auf 80/8080):"
ss -ltnp 2>/dev/null | grep -E ':(80|8080)\b' || echo "  (nichts auf 80/8080 -> kein separater Datei-Dienst)"
