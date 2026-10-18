#!/bin/sh
# AP+STA on vendor rtl88x2bu (concurrent build), P2P disabled so only 2 VIFs exist.
# STA VIF stays with NetworkManager (autoconnect), AP VIF is unmanaged + hostapd.
IF=wlxdc627949ba79     # STA VIF (dc:62...) - NM
AP=wlxde627949ba79     # AP  VIF (de:62...) - hostapd
LOG=/root/netbackup/apsta-run.log
exec >"$LOG" 2>&1
echo "================ $(date) AP+STA run ================"

echo "-- 0. state --"
nmcli -t -f DEVICE,STATE,CONNECTION dev status | grep wlx

echo "-- 1. apply p2p off, restart NM --"
systemctl restart NetworkManager
sleep 8
nmcli -t -f DEVICE,STATE,CONNECTION dev status | grep wlx

echo "-- 2. STA connected? --"
ip -br addr show "$IF"
iw dev "$IF" info 2>/dev/null | grep -E 'ssid|type|channel'

echo "-- 3. AP VIF out of NM, static IP, hostapd --"
nmcli dev set "$AP" managed no 2>/dev/null
ip link set "$AP" up
ip addr replace 10.42.0.1/24 dev "$AP"
cat > /etc/hostapd-odroid.conf <<EOF
interface=$AP
driver=nl80211
ssid=Odroid
hw_mode=a
channel=36
ieee80211n=1
country_code=DE
EOF
pkill -9 hostapd 2>/dev/null; sleep 1
setsid hostapd /etc/hostapd-odroid.conf > /var/log/hostapd-odroid.log 2>&1 &
sleep 8
grep -E 'AP-ENABLED|AP-DISABLED|Failed|Could not' /var/log/hostapd-odroid.log | tail -3
iw dev "$AP" info 2>/dev/null | grep -E 'type|ssid|channel'

echo "-- 4. BOTH at once? --"
echo -n "STA ssid: "; iw dev "$IF" info 2>/dev/null | awk '/ssid/{print substr($0,index($0,"ssid")+5)}'
echo -n "AP  ssid: "; iw dev "$AP" info 2>/dev/null | awk '/ssid/{print substr($0,index($0,"ssid")+5)}'
ip -br addr show "$IF"
ip -br addr show "$AP"
echo -n "STA->gateway: "; ping -c2 -W2 -I "$IF" 192.168.178.1 >/dev/null 2>&1 && echo OK || echo FAIL
echo -n "AP  self:     "; ping -c2 -W2 -I "$AP" 10.42.0.1 >/dev/null 2>&1 && echo OK || echo FAIL
echo "-- dmesg tail --"
dmesg 2>/dev/null | tail -6
echo "================ $(date) done ================"
