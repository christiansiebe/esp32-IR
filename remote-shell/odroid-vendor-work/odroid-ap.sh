#!/bin/sh
# odroid-ap.sh - standalone "Odroid" access point (in-kernel rtw88).
#
# SAFETY: in-kernel rtw88 cannot do AP+STA concurrently, so this takes the
# WLAN station down. It therefore refuses to run unless the Ethernet port
# (end0) has carrier, so a reboot without the LAN cable can never lock the
# board out of the network (the WLAN STA then stays up under NetworkManager).
#
# The in-kernel driver needs the AP VIF created, given a UNIQUE MAC and
# brought up BEFORE hostapd starts (else: "Name not unique on network").
set -e
IF=wlxdc627949ba79
AP=ap0
LAN=end0

# wait up to 30 s for LAN carrier
i=0
while [ "$i" -lt 30 ]; do
    [ "$(cat /sys/class/net/$LAN/carrier 2>/dev/null)" = "1" ] && break
    i=$((i+1)); sleep 1
done
if [ "$(cat /sys/class/net/$LAN/carrier 2>/dev/null)" != "1" ]; then
    echo "no LAN carrier on $LAN - refusing to start AP (would drop the only link)"
    exit 0
fi

[ -d "/sys/class/net/$IF" ] || IF=$(ls -d /sys/class/net/wlx* 2>/dev/null | head -1 | xargs -r basename)
[ -n "$IF" ] || { echo "no wlan interface found"; exit 1; }

pkill -9 hostapd 2>/dev/null || true
sleep 1
iw dev "$AP" del 2>/dev/null || true
nmcli dev set "$IF" managed no 2>/dev/null || true
ip link set "$IF" down 2>/dev/null || true
sleep 1

iw dev "$IF" interface add "$AP" type __ap
ip link set "$AP" address dc:62:79:49:ba:7a
ip link set "$AP" up
ip addr replace 10.42.0.1/24 dev "$AP"

exec hostapd -dd /etc/hostapd-odroid.conf
