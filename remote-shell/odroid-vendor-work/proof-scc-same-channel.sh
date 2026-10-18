#!/bin/sh
# conc-apfirst.sh - the decisive concurrent test.
#
# The AP-only service already holds channel 36. The FB7272 uplink
# ("Wohnzimmer 5 Ghz") is ALSO on channel 36. So bringing the STA up now must
# succeed if the driver does single-channel concurrency. This is the "AP first,
# then STA" ordering the driver needs (the STA must not own the radio when
# hostapd sets the channel).
set -u
AP=wlxde627949ba79
STA=wlxdc627949ba79
NM_CONF=/etc/NetworkManager/conf.d/99-odroid-ap-unmanaged.conf

echo "=== 0. state: AP service is up on channel 36 ==="
iw dev "$AP" info | grep -E 'ssid|type'
grep -E '^channel' /etc/hostapd-odroid.conf

echo "=== 1. allow the STA in NetworkManager (keep the AP untouched) ==="
[ -f "$NM_CONF" ] && cp "$NM_CONF" /tmp/nm-conf.bak
rm -f "$NM_CONF"
nmcli general reload 2>/dev/null || systemctl reload NetworkManager
sleep 3
nmcli dev set "$STA" managed yes 2>/dev/null || true
nmcli dev set "p2p-dev-$STA" managed no 2>/dev/null || true
sleep 2

echo "=== 2. connect the STA to the FB7272 (AP keeps running) ==="
nmcli con up 'Supervisor wlxdc627949ba79' 2>&1 | tail -1
sleep 15
echo "--- STA ---"
iw dev "$STA" link 2>/dev/null | grep -E 'Connected to|SSID|freq|signal'

echo "--- AP still alive? ---"
iw dev "$AP" info | grep -E 'ssid|type'
echo -n "hostapd: "; pgrep -c hostapd
echo "--- AP hostapd log tail ---"
journalctl -u odroid-apsta.service -n 6 --no-pager 2>/dev/null | tail -6

echo
echo "=== 3. verdict ==="
if iw dev "$STA" link 2>/dev/null | grep -q 'Connected to' && iw dev "$AP" info 2>/dev/null | grep -q 'type AP'; then
    echo "CONCURRENT OK: STA and AP are BOTH up"
else
    echo "CONCURRENT FAILED: one side is down"
fi

echo
echo "=== 4. keep it running; report channels ==="
echo -n "AP channel : "; iw dev "$AP" info 2>/dev/null | awk '/channel/{print $2}'
echo -n "STA freq   : "; iw dev "$STA" link 2>/dev/null | awk '/freq/{print $2}'
echo "=== DONE ==="
