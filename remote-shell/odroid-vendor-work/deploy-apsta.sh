#!/bin/sh
# deploy-apsta.sh - make vendor rtl88x2bu + "Odroid" AP persistent across reboot.
# Idempotent. LAN (end0) stays the safety net.
set -e
KREL=$(uname -r)
KO=/root/netbackup/88x2bu-gcc14-concurrent-2026.ko

echo "=== 1. install the module for boot ==="
mkdir -p "/lib/modules/$KREL/extra"
cp "$KO" "/lib/modules/$KREL/extra/88x2bu.ko"
depmod -a
echo "installed: $(ls -la /lib/modules/$KREL/extra/88x2bu.ko)"

echo "=== 2. blacklist in-kernel rtw88 + set powersave options ==="
cat > /etc/modprobe.d/88x2bu.conf <<'EOF'
# Vendor Realtek rtl88x2bu (TP-Link 2357:0138). Blacklist the in-kernel rtw88
# so the vendor driver binds the stick (needed for concurrent AP+STA).
blacklist rtw88_8822bu
# Power saving: rtw_power_mgnt 0=off 1=min 2=max. IPS OFF (rtw_ips_mode=0) so
# an idle AP never powers the radio down under its clients.
options 88x2bu rtw_power_mgnt=2 rtw_ips_mode=0 rtw_smart_ps=2 rtw_enusbss=0
EOF

echo "=== 3. load at boot ==="
echo 88x2bu > /etc/modules-load.d/88x2bu.conf

echo "=== 4. AP script ==="
cat > /usr/local/sbin/odroid-apsta.sh <<'EOF'
#!/bin/sh
# odroid-apsta.sh - "Odroid" AP on the vendor rtl88x2bu, concurrent with the
# WLAN station. STA VIF stays under NetworkManager (uplink); the second VIF
# becomes the AP on the same channel (concurrent mode). Never touches STA.
set -e

i=0
while [ "$i" -lt 30 ]; do
    n=$(ls /sys/class/net 2>/dev/null | grep -c '^wlx')
    [ "$n" -ge 2 ] && break
    i=$((i+1)); sleep 1
done

STA=$(nmcli -t -f DEVICE,STATE dev status 2>/dev/null | awk -F: '/^wlx/ && $2=="connected"{print $1; exit}')
ALL=$(ls /sys/class/net | grep '^wlx')
[ -n "$STA" ] || STA=$(echo "$ALL" | head -1)
AP=""
for i in $ALL; do [ "$i" != "$STA" ] && { AP="$i"; break; }; done
[ -n "$AP" ] || { echo "no second VIF - vendor driver (concurrent) not loaded?"; exit 1; }
echo "STA=$STA AP=$AP"

nmcli dev set "$AP" managed no 2>/dev/null || true
nmcli dev set "p2p-dev-$AP" managed no 2>/dev/null || true
ip addr flush dev "$AP" 2>/dev/null || true
ip link set "$AP" up

CH=$(iw dev "$STA" info 2>/dev/null | awk '/channel/{print $2}')
[ -n "$CH" ] || CH=36
case "$CH" in 1|2|3|4|5|6|7|8|9|10|11|12|13) HW=b ;; *) HW=a ;; esac
echo "AP channel=$CH hw_mode=$HW"

cat > /etc/hostapd-odroid.conf <<CONF
interface=$AP
driver=nl80211
ssid=Odroid
hw_mode=$HW
channel=$CH
ieee80211n=1
country_code=DE
CONF

# DHCP for AP clients, bound to the detected AP interface
cat > /etc/dnsmasq.d/odroid-ap.conf <<CONF
interface=$AP
bind-interfaces
port=0
dhcp-range=10.42.0.10,10.42.0.200,255.255.255.0,12h
dhcp-option=3,10.42.0.1
dhcp-option=6,10.42.0.1
CONF

ip addr replace 10.42.0.1/24 dev "$AP"
systemctl restart odroid-apsta-dnsmasq.service 2>/dev/null || true

pkill -9 hostapd 2>/dev/null || true
sleep 1
exec hostapd /etc/hostapd-odroid.conf
EOF
chmod 755 /usr/local/sbin/odroid-apsta.sh

echo "=== 5. AP + dnsmasq services ==="
cat > /etc/systemd/system/odroid-apsta.service <<'EOF'
[Unit]
Description=Odroid "Odroid" AP on the vendor rtl88x2bu (concurrent with STA)
After=network-online.target NetworkManager.service
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/odroid-apsta.sh
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/odroid-apsta-dnsmasq.service <<'EOF'
[Unit]
Description=DHCP for the Odroid AP (dnsmasq on the AP interface)
PartOf=odroid-apsta.service
After=odroid-apsta.service

[Service]
Type=simple
ExecStart=/usr/sbin/dnsmasq --no-daemon --conf-file=/etc/dnsmasq.d/odroid-ap.conf
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable odroid-apsta.service odroid-apsta-dnsmasq.service 2>&1 | tail -4
echo "=== done (enabled, not started) ==="
