#!/bin/sh
# With LAN (end0) as uplink, run a STANDALONE AP "Odroid" (STA off).
# No auto-restore needed: the command channel goes over end0 now.
# Writes everything to a log so we can inspect even if the channel blips.
LOG=/root/netbackup/ap-lan.log
IF=wlxdc627949ba79
SSID='Supervisor wlxdc627949ba79'
exec >>"$LOG" 2>&1
echo "================ $(date) AP-on-LAN ================"

echo "--- sanity: LAN up? ---"
ip -br addr show end0
ip route show default

echo "--- kill leftover hostapd, STA down ---"
pkill -9 hostapd 2>/dev/null
sleep 1
nmcli con down "$SSID" 2>/dev/null
sleep 3

echo "--- create ap0 ---"
iw dev ap0 del 2>/dev/null
sleep 1
iw dev "$IF" interface add ap0 type __ap; echo "add rc=$?"
ip link set ap0 address dc:62:79:49:ba:7a
ip link set ap0 up; echo "pre-up rc=$?"

cat > /etc/hostapd-odroid.conf <<'EOF'
interface=ap0
driver=nl80211
ssid=Odroid
hw_mode=g
channel=6
ieee80211n=1
country_code=DE
EOF
setsid hostapd /etc/hostapd-odroid.conf > /var/log/hostapd-odroid.log 2>&1 &
sleep 5
echo "--- AP state ---"
ip link show ap0 | head -1
iw dev ap0 info 2>/dev/null | grep -E 'type|channel|ssid'
grep -E 'AP-ENABLED|AP-DISABLED|Could not configure' /var/log/hostapd-odroid.log | tail -2
ip addr add 10.42.0.1/24 dev ap0 2>/dev/null
ip -br addr show ap0

echo "--- verify STA is really down, LAN still default ---"
ip route show default
ping -c2 -W2 192.168.178.1 >/dev/null 2>&1 && echo "LAN-PING-OK" || echo "LAN-PING-FAILED"
echo "=== AP 'Odroid' running standalone on 2.4GHz ch6 ==="
echo "================ $(date) ================"
