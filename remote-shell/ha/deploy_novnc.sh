#!/bin/sh
# Installiert den XFCE-Desktop im Browser (TigerVNC + noVNC/websockify).
#
# Ergebnis: http://odroidn2.local:6080/vnc.html zeigt den echten XFCE-Desktop.
# Reboot-fest (systemd, enabled). Reines Host-Tooling, kein Firmware-Bezug.
#
# Aufruf: deploy_novnc.sh
#
# WICHTIG: /vnc.html nicht weglassen. ":6080/" allein zeigt die noVNC-
# Verzeichnisliste von /usr/share/novnc/ - das sieht wie ein Datei-Explorer
# aus, ist aber NICHT der Desktop.
set -eu

HERE=$(dirname "$(readlink -f "$0")")

for f in xfce-vnc.service novnc.service vnc-xstartup; do
	[ -f "$HERE/$f" ] || { echo "FEHLER: $HERE/$f fehlt"; exit 1; }
done

echo '== 1. Pakete =='
if ! dpkg -s tigervnc-standalone-server >/dev/null 2>&1 || \
   ! dpkg -s novnc websockify >/dev/null 2>&1; then
	apt-get update -y
	apt-get install -y tigervnc-standalone-server xfce4 dbus-x11 novnc websockify
else
	echo '   tigervnc/xfce4/novnc/websockify schon installiert'
fi

echo '== 2. VNC-Startskript =='
mkdir -p /root/.vnc
install -m 0755 "$HERE/vnc-xstartup" /root/.vnc/xstartup

echo '== 3. VNC-Passwort (nur Host, nie ins Repo) =='
if [ ! -s /root/.vnc/passwd ]; then
	# Nicht-interaktiv setzen; Klartext-Notiz liegt unter /root/.vnc/PASSWORT.txt.
	PW=$(python3 -c "import secrets,string;print(''.join(secrets.choice(string.ascii_letters+string.digits) for _ in range(8)))")
	printf '%s\n' "$PW" | vncpasswd -f > /root/.vnc/passwd
	chmod 600 /root/.vnc/passwd
	printf '%s\n' "$PW" > /root/.vnc/PASSWORT.txt
	chmod 600 /root/.vnc/PASSWORT.txt
	echo "   neues Passwort gesetzt -> /root/.vnc/PASSWORT.txt"
else
	echo '   vorhandenes Passwort bleibt'
fi

echo '== 4. systemd-Units =='
install -m 0644 "$HERE/xfce-vnc.service" /etc/systemd/system/xfce-vnc.service
install -m 0644 "$HERE/novnc.service"    /etc/systemd/system/novnc.service
systemctl daemon-reload
systemctl enable --now xfce-vnc.service
systemctl enable --now novnc.service

sync

echo '== 5. Zustand =='
sleep 2
systemctl is-active xfce-vnc.service novnc.service
curl -s -o /dev/null -w 'vnc.html HTTP=%{http_code}\n' --max-time 5 http://localhost:6080/vnc.html || true
echo "== fertig: http://$(hostname -I | awk '{print $1}'):6080/vnc.html =="
