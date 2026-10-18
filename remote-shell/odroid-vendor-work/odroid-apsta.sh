#!/bin/sh
# odroid-apsta.sh - "Odroid" AP on the vendor rtl88x2bu, concurrent with the
# WLAN station. The STA VIF stays under NetworkManager (uplink); the second
# VIF becomes the AP. The AP channel follows the STA (concurrent mode needs
# both on the same channel). Safety: never touches the STA VIF.
set -e

# --- wait until the vendor driver has created two wlx VIFs (boot race) ---
i=0
while [ "$i" -lt 30 ]; do
    n=$(ls /sys/class/net 2>/dev/null | grep -c '^wlx')
    [ "$n" -ge 2 ] && break
    i=$((i+1)); sleep 1
done

# --- pick the two VIFs: STA = the one NM has connected, AP = the other ---
STA=$(nmcli -t -f DEVICE,STATE dev status 2>/dev/null | awk -F: '/^wlx/ && $2=="connected"{print $1; exit}')
ALL=$(ls /sys/class/net | grep '^wlx')
[ -n "$STA" ] || STA=$(echo "$ALL" | head -1)
AP=""
for i in $ALL; do [ "$i" != "$STA" ] && { AP="$i"; break; }; done
[ -n "$AP" ] || { echo "no second VIF - vendor driver (concurrent) not loaded?"; exit 1; }
echo "STA=$STA AP=$AP"

# --- take the AP VIF (and its p2p sibling) out of NetworkManager ---
nmcli dev set "$AP" managed no 2>/dev/null || true
nmcli dev set "p2p-dev-$AP" managed no 2>/dev/null || true
ip addr flush dev "$AP" 2>/dev/null || true
ip link set "$AP" up

# --- match the AP channel to the STA (MCC off => same channel required) ---
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

ip addr replace 10.42.0.1/24 dev "$AP"

# --- hostapd in the foreground so systemd tracks it (Restart=always) ---
pkill -9 hostapd 2>/dev/null || true
sleep 1
exec hostapd /etc/hostapd-odroid.conf
