#!/bin/sh
# Macht den XFCE-Desktop passwortfrei, aber NUR aus dem LAN erreichbar.
#
# Zwei Schichten (siehe vnc_lan_guard.py):
#   1. TigerVNC hoert nur auf 127.0.0.1  -> Port 5901 von aussen unerreichbar.
#   2. vnc-lan-guard: eigener Paketfilter auf Port 6080, nur LAN + Loopback.
#
# Aufruf: deploy_vnc_lan.sh [LAN-CIDR]     (Default: aus vnc-lan-guard.default)
#
# Idempotent. ACHTUNG: ab hier hat der Desktop KEIN Passwort mehr - der Schutz
# ist ausschliesslich die Quell-IP-Pruefung. Das LAN muss vertrauenswuerdig sein.
set -eu

HERE=$(dirname "$(readlink -f "$0")")
LAN_CIDR="${1:-}"

for f in vnc_lan_guard.py vnc-lan-guard.service vnc-lan-guard.default \
         xfce-vnc.service novnc.service; do
	[ -f "$HERE/$f" ] || { echo "FEHLER: $HERE/$f fehlt"; exit 1; }
done

command -v iptables >/dev/null 2>&1 || { echo "FEHLER: iptables fehlt"; exit 1; }

if [ -z "$LAN_CIDR" ]; then
	LAN_CIDR=$(sed -n 's/^LAN_CIDR=//p' "$HERE/vnc-lan-guard.default" | head -1)
fi
echo "== LAN: $LAN_CIDR (alles andere wird auf Port 6080 verworfen) =="

echo '== 1. Guard-Skript + Konfiguration =='
install -m 0755 "$HERE/vnc_lan_guard.py" /usr/local/sbin/vnc_lan_guard.py
install -m 0644 "$HERE/vnc-lan-guard.default" /etc/default/vnc-lan-guard
if [ -n "${1:-}" ]; then
	sed -i "s|^LAN_CIDR=.*|LAN_CIDR=$1|" /etc/default/vnc-lan-guard
fi

echo '== 2. Units =='
install -m 0644 "$HERE/vnc-lan-guard.service" /etc/systemd/system/vnc-lan-guard.service
install -m 0644 "$HERE/xfce-vnc.service"      /etc/systemd/system/xfce-vnc.service
install -m 0644 "$HERE/novnc.service"         /etc/systemd/system/novnc.service
systemctl daemon-reload

echo '== 3. Guard aktivieren (Filter ZUERST, dann Passwort weg) =='
systemctl enable --now vnc-lan-guard.service
sleep 2

echo '== 4. VNC neu starten (ohne Passwort, nur Loopback) =='
systemctl restart xfce-vnc.service
systemctl restart novnc.service
sleep 2

sync

echo '== 5. Zustand =='
/usr/local/sbin/vnc_lan_guard.py --print
echo '-- Bindings (5901 muss 127.0.0.1 sein) --'
ss -tlnp 2>/dev/null | grep -E ':5901|:6080' || true
echo '-- Filterkette --'
iptables -S VNC_LAN 2>/dev/null || echo '(Kette fehlt!)'
iptables -S INPUT | grep VNC_LAN || echo '(INPUT-Sprung fehlt!)'
echo '-- Dienste --'
systemctl is-active xfce-vnc.service novnc.service vnc-lan-guard.service
echo "== fertig: http://odroidn2.local:6080/vnc.html (nur LAN) =="
