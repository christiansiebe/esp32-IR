#!/bin/sh
# scc-proof2.sh - fixed version. Convert the STA frequency to a channel number
# (hostapd wants the channel, not MHz) and capture the full hostapd error log.
set -u
AP=wlxde627949ba79
STA=wlxdc627949ba79
NM_CONF=/etc/NetworkManager/conf.d/99-odroid-ap-unmanaged.conf

freq2chan() {
    # 5 GHz: chan = (freq - 5000) / 5
    echo $(( ($1 - 5000) / 5 ))
}

echo "=== 0. stop AP service ==="
systemctl stop odroid-apsta.service 2>/dev/null || true
pkill -9 hostapd 2>/dev/null || true
sleep 4

echo "=== 1. allow STA in NM ==="
[ -f "$NM_CONF" ] && cp "$NM_CONF" /tmp/nm-conf.bak
rm -f "$NM_CONF"
nmcli general reload 2>/dev/null || systemctl reload NetworkManager
sleep 3

echo "=== 2. STA up to FB7272 ==="
nmcli dev set "$STA" managed yes 2>/dev/null || true
sleep 2
nmcli con up 'Supervisor wlxdc627949ba79' 2>&1 | tail -1
sleep 15
iw dev "$STA" link 2>/dev/null | grep -E 'Connected to|SSID|freq|signal'
FR=$(iw dev "$STA" link 2>/dev/null | awk '/freq:/{print $2}')
CH=$(freq2chan "$FR")
echo "STA freq=$FR  -> channel=$CH"

run_ap() {  # $1=channel  $2=logfile
    cat > /tmp/scc.conf <<CONF
interface=$AP
driver=nl80211
ssid=OdroidTest
hw_mode=a
channel=$1
country_code=DE
ieee80211n=1
wmm_enabled=1
ht_capab=[HT20][SHORT-GI-20]
CONF
    : > "$2"
    nohup hostapd -dd -f "$2" /tmp/scc.conf >/dev/null 2>&1 &
    sleep 10
    echo "hostapd running: $(pgrep -c hostapd)"
    grep -inE 'AP-ENABLED|Failed to set channel|Could not set channel|busy|Channel .* not|invalid' "$2" | tail -4
    pkill -9 hostapd 2>/dev/null || true
    sleep 5
}

echo
echo "=== 3. test A: AP on the SAME channel as STA ($CH) ==="
run_ap "$CH" /tmp/sccA.log

echo
echo "=== 4. test B: AP on a DIFFERENT channel (44) ==="
run_ap 44 /tmp/sccB.log

echo
echo "=== 5. restore ==="
[ -f /tmp/nm-conf.bak ] && cp /tmp/nm-conf.bak "$NM_CONF"
nmcli general reload 2>/dev/null || systemctl reload NetworkManager
nmcli dev set "$STA" managed no 2>/dev/null || true
sleep 2
systemctl start odroid-apsta.service
sleep 25
systemctl is-active odroid-apsta.service odroid-apsta-dnsmasq.service odroid-apsta-nat.service
iw dev "$AP" info | grep -E 'ssid|type'
echo "=== DONE ==="
