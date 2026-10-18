#!/bin/sh
# powersave.sh - coherent power config for the Odroid AP+STA setup.
#
# Intent:
#   STA VIF (uplink)  -> powersave ON  (save power, a few ms latency is fine)
#   AP  VIF (clients) -> powersave OFF (must stay awake for its clients)
# Also: NM global stays Armbian's "disable" (2); the STA profile overrides it
# per-connection. rc.local is fixed to not fight NM on the STA.
set -e
IF=wlxdc627949ba79
AP=wlxde627949ba79

echo "=== BEFORE ==="
echo -n "STA ps: "; iw dev "$IF" get power_save
echo -n "AP  ps: "; iw dev "$AP" get power_save
echo -n "NM global: "; grep -h powersave /etc/NetworkManager/conf.d/zz-20-*.conf

echo
echo "=== 1. STA profile: powersave ON (per-connection overrides global) ==="
nmcli con mod 'Supervisor wlxdc627949ba79' 802-11-wireless.powersave 3
nmcli -g 802-11-wireless.powersave con show 'Supervisor wlxdc627949ba79'

echo
echo "=== 2. rc.local: target the STA VIF explicitly, and leave the AP alone ==="
cat > /etc/rc.local <<'EOF'
#!/bin/sh
# rc.local - energy settings (CPU governor + WLAN powersave on the STA VIF).
for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
    [ -w "$g" ] && echo "schedutil" > "$g"
done

RL=/sys/devices/system/cpu/cpufreq/schedutil/rate_limit_us
[ -w "$RL" ] && echo 10 > "$RL"

# Powersave only on the STA VIF (uplink). The AP VIF must stay awake and is
# handled by odroid-apsta.sh. Pick the VIF NetworkManager has connected.
STA=$(nmcli -t -f DEVICE,STATE dev status 2>/dev/null | awk -F: '/^wlx/ && $2=="connected"{print $1; exit}')
[ -n "$STA" ] && /usr/sbin/iw dev "$STA" set power_save on 2>/dev/null

exit 0
EOF
chmod 755 /etc/rc.local

echo
echo "=== 3. AP script: force powersave OFF on the AP VIF ==="
if ! grep -q 'set power_save off' /usr/local/sbin/odroid-apsta.sh; then
    sed -i 's|^ip link set "\$AP" up$|ip link set "$AP" up\n/usr/sbin/iw dev "$AP" set power_save off 2>/dev/null \|\| true|' /usr/local/sbin/odroid-apsta.sh
fi
grep -n 'power_save' /usr/local/sbin/odroid-apsta.sh || echo "(no match - check manually)"

echo
echo "=== 4. apply now ==="
/usr/sbin/iw dev "$AP" set power_save off 2>/dev/null || true
nmcli con up 'Supervisor wlxdc627949ba79' >/dev/null 2>&1 || true
sleep 6
echo -n "STA ps: "; iw dev "$IF" get power_save
echo -n "AP  ps: "; iw dev "$AP" get power_save

echo
echo "=== 5. latency (STA -> gateway) ==="
ping -c8 -W2 -I "$IF" 192.168.178.1 2>/dev/null | tail -2

echo "=== done ==="
